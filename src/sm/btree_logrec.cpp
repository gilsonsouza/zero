/*
 * (c) Copyright 2011-2014, Hewlett-Packard Development Company, LP
 */

/**
 * Logging and its UNDO/REDO code for BTrees.
 * Separated from logrec.cpp.
 */

#include "btree_logrec.h"
#include "vol.h"
#include "bf_tree_cb.h"

btree_insert_t::btree_insert_t(
    PageID root_pid,
    const w_keystr_t&     key,
    const cvec_t&         el,
    const bool            is_sys_txn)
    : klen(key.get_length_as_keystr()), elen(el.size())
{
    root_shpid = root_pid;
    w_assert1((size_t)(klen + elen) < sizeof(data));
    key.serialize_as_keystr(data);
    el.copy_to(data + klen);
    sys_txn = is_sys_txn;
}

/**
 * A \b multi-page \b SSX log record for \b btree_foster_adopt.
 * This log is totally \b self-contained, so no WOD assumed.
 */
btree_foster_adopt_t::btree_foster_adopt_t(PageID page2_id, PageID new_child_pid,
                        lsn_t new_child_emlsn, const w_keystr_t& new_child_key)
    : multi_page_log_t(page2_id), _new_child_emlsn(new_child_emlsn),
    _new_child_pid (new_child_pid) {
    _new_child_key_len = new_child_key.get_length_as_keystr();
    new_child_key.serialize_as_keystr(_data);
}

/**
 * A \b multi-page \b SSX log record for \b btree_foster_deadopt.
 * This log is totally \b self-contained, so no WOD assumed.
 */
btree_foster_deadopt_t::btree_foster_deadopt_t(PageID page2_id, PageID deadopted_pid,
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

/**
 * A \b multi-page \b SSX log record for \b btree_foster_merge.
 * This log is \b NOT-self-contained, so it assumes WOD (foster-child is deleted later).
 */
btree_foster_merge_t::btree_foster_merge_t(PageID page2_id,
        const w_keystr_t& high,            // high (foster) of destination
        const w_keystr_t& chain_high,      // high fence of all foster nodes
        PageID foster_pid0,               // foster page id in destination page
        lsn_t foster_pid0_emlsn,           // foster emlsn in destination page
        const int16_t prefix_len,          // source page prefix length
        const int32_t move_count,          // number of records to be moved
        const smsize_t record_data_len,    // the data length in record_data, for data copy purpose
        const cvec_t& record_data)         // the actual data records for all the moved records,
                                           // self contained record buffer, meaning each reocrd is in the format:
                                           // ghost flag + key length + key (with sign byte) + child + ghost flag + data length + data
    : multi_page_log_t(page2_id) {

    w_assert1(size() < logrec_t::max_data_sz);

    _move_count = move_count;
    _foster_pid0   = foster_pid0;
    _foster_pid0_emlsn = foster_pid0_emlsn;
    _prefix_len = prefix_len;

    // Figure out the size of each data field
    _high_len = (uint16_t)high.get_length_as_keystr();               // keystr, including sign byte
    _chain_high_len = (uint16_t)chain_high.get_length_as_keystr();   // keystr, including sign byte
    _record_data_len = record_data_len;

    // Put all data fields into _data
    high.serialize_as_keystr(_data);
    chain_high.serialize_as_keystr(_data + _high_len);

    // Copy all the record data into _data
    record_data.copy_to(_data + _high_len + _chain_high_len, _record_data_len);

}

/**
 * A \b multi-page \b SSX log record for \b btree_foster_rebalance.
 * This log is \b NOT-self-contained, so it assumes WOD (foster-parent is written later).
 */
btree_foster_rebalance_t::btree_foster_rebalance_t(
        PageID page2_id,                 // data source (foster parent page)
        const w_keystr_t& fence,          // low fence of destination, also high (foster) of source
        PageID new_pid0, lsn_t new_pid0_emlsn,
        const w_keystr_t& high,           // high (foster) of destination
        const w_keystr_t& chain_high,     // high fence of all foster nodes
        const int16_t prefix_len,         // source page prefix length
        const int32_t move_count,         // number of records to be moved
        const smsize_t record_data_len,   // the data length in record_data, for data copy purpose
        const cvec_t& record_data)        // the actual data records for all the moved records,
                                          // self contained record buffer, meaning each reocrd is in the format:
                                          // ghost flag + key length + key (with sign byte) + child + ghost flag + data length + data
    : multi_page_log_t(page2_id) {
    _move_count = move_count;
    _new_pid0   = new_pid0;
    _new_pid0_emlsn = new_pid0_emlsn;
    _prefix_len = prefix_len;

    // Figure out the size of each data field
    _fence_len = (uint16_t)fence.get_length_as_keystr();           // keystr, including sign byte
    _high_len = (uint16_t)high.get_length_as_keystr();             // keystr, including sign byte
    _chain_high_len = (uint16_t)chain_high.get_length_as_keystr(); // keystr, including sign byte
    _record_data_len = record_data_len;

    w_assert1(size() < logrec_t::max_data_sz);

    // Put all data fields into _data
    fence.serialize_as_keystr(_data);
    high.serialize_as_keystr(_data + _fence_len);
    chain_high.serialize_as_keystr(_data + _fence_len + _high_len);

    // Copy all the record data into _data
    record_data.copy_to(_data + _fence_len + _high_len + _chain_high_len, _record_data_len);

}

btree_ghost_reserve_t::btree_ghost_reserve_t(const w_keystr_t& key, int elem_length)
    : klen (key.get_length_as_keystr()), element_length (elem_length)
{
    key.serialize_as_keystr(data);
}

template <class PagePtr>
btree_ghost_t<PagePtr>::btree_ghost_t
(const PagePtr p, const vector<slotid_t>& slots, const bool is_sys_txn)
{
    root_shpid = p->root();
    cnt = slots.size();
    if (true == is_sys_txn)
        sys_txn = 1;
    else
        sys_txn = 0;
    uint16_t *offsets = reinterpret_cast<uint16_t*>(slot_data);
    char *current = slot_data + sizeof (uint16_t) * slots.size();

    // the first data is prefix
    {
        uint16_t prefix_len = p->get_prefix_length();
        prefix_offset = (current - slot_data);
        // *reinterpret_cast<uint16_t*>(current) = prefix_len; this causes Bus Error on solaris! so, instead:
        ::memcpy(current, &prefix_len, sizeof(uint16_t));
        if (prefix_len > 0) {
            ::memcpy(current + sizeof(uint16_t), p->get_prefix_key(), prefix_len);
        }
        current += sizeof(uint16_t) + prefix_len;
    }

     for (size_t i = 0; i < slots.size(); ++i) {
        size_t len;
        w_assert3(p->is_leaf()); // ghost exists only in leaf
        const char* key = p->_leaf_key_noprefix(slots[i], len);
        offsets[i] = (current - slot_data);
        // *reinterpret_cast<uint16_t*>(current) = len; this causes Bus Error on solaris! so, instead:
        uint16_t len_u16 = (uint16_t) len;
        ::memcpy(current, &len_u16, sizeof(uint16_t));
        ::memcpy(current + sizeof(uint16_t), key, len);
        current += sizeof(uint16_t) + len;
    }
    total_data_size = current - slot_data;
    w_assert0(logrec_t::max_data_sz >= sizeof(PageID) + sizeof(uint16_t) * 2  + sizeof(size_t) + total_data_size);
}

template <class T>
w_keystr_t btree_ghost_t<T>::get_key (size_t i) const {
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

/**
 * A \b multi-page \b SSX log record for \b btree_norec_alloc.
 * This log is totally \b self-contained, so no WOD assumed.
 */
template <class Ptr>
btree_norec_alloc_t<Ptr>::btree_norec_alloc_t(const Ptr p,
        PageID new_page_id, const w_keystr_t& fence, const w_keystr_t& chain_fence_high)
    : multi_page_log_t(new_page_id) {
    w_assert1 (smthread_t::xct()->is_single_log_sys_xct());
    w_assert1 (new_page_id != p->btree_root());
    w_assert1 (p->latch_mode() != LATCH_NL);

    _root_pid       = p->btree_root();
    _foster_pid     = p->get_foster();
    _foster_emlsn   = p->get_foster_emlsn();
    _fence_len      = (uint16_t) fence.get_length_as_keystr();
    _chain_high_len = (uint16_t) chain_fence_high.get_length_as_keystr();
    _btree_level    = (int16_t) p->level();
    w_assert1(size() < logrec_t::max_data_sz);

    fence.serialize_as_keystr(_data);
    chain_fence_high.serialize_as_keystr(_data + _fence_len);
}


template <class PagePtr>
void btree_insert_log::construct(
    const PagePtr page,
    const w_keystr_t&   key,
    const cvec_t&       el,
    const bool          is_sys_txn)
{
    header._cat = 0|t_redo|t_undo|t_logical, header._type = t_btree_insert;
    fill(page,
         (new (_data) btree_insert_t(page->root(), key, el, is_sys_txn))->size());
}

template <class PagePtr>
void btree_insert_log::undo(PagePtr page) {
    w_assert9(page == 0);
    btree_insert_t* dp = (btree_insert_t*) data();

    if (true == dp->sys_txn)
    {
        // The insertion log record was generated by a page rebalance full logging operation
        // no 'undo' in this case
        return;
    }

    w_keystr_t key;
    key.construct_from_keystr(dp->data, dp->klen);

// TODO(Restart)...
DBGOUT3( << "&&&& UNDO insertion, key: " << key);

    // ***LOGICAL*** don't grab locks during undo
    W_COERCE(smlevel_0::bt->remove_as_undo(header._stid, key));
}

template <class PagePtr>
void btree_insert_log::redo(PagePtr page) {
    borrowed_btree_page_h bp(page);
    btree_insert_t* dp = (btree_insert_t*) data();

    w_assert1(bp.is_leaf());
    w_keystr_t key;
    vec_t el;
    key.construct_from_keystr(dp->data, dp->klen);
    el.put(dp->data + dp->klen, dp->elen);

// TODO(Restart)...
DBGOUT3( << "&&&& REDO insertion by replace ghost, key: " << key);

    // PHYSICAL redo
    // see btree_impl::_ux_insert()
    // at the point we called log_btree_insert,
    // we already made sure the page has a ghost
    // record for the key that is enough spacious.
    // so, we just replace the record!
    DBGOUT3( << "btree_insert_log::redo - key to replace ghost: " << key);
    w_rc_t rc = bp.replace_ghost(key, el, true /* redo */);
    if(rc.is_error()) { // can't happen. wtf?
        W_FATAL_MSG(fcINTERNAL, << "btree_insert_log::redo " );
    }
}

template <class PagePtr>
void btree_insert_nonghost_log::construct(
    const PagePtr page, const w_keystr_t &key, const cvec_t &el, const bool is_sys_txn) {
    header._cat = 0|t_redo|t_undo|t_logical, header._type = t_btree_insert_nonghost;
    fill(page,
        (new (_data) btree_insert_t(page->root(), key, el, is_sys_txn))->size());
}

template <class PagePtr>
void btree_insert_nonghost_log::undo(PagePtr page) {
    reinterpret_cast<btree_insert_log*>(this)->undo(page); // same as btree_insert
}

template <class PagePtr>
void btree_insert_nonghost_log::redo(PagePtr page) {
    borrowed_btree_page_h bp(page);
    btree_insert_t* dp = reinterpret_cast<btree_insert_t*>(data());

    w_assert1(bp.is_leaf());
    w_keystr_t key;
    vec_t el;
    key.construct_from_keystr(dp->data, dp->klen);
    el.put(dp->data + dp->klen, dp->elen);

// TODO(Restart)...
DBGOUT3( << "&&&& REDO insertion, key: " << key);

    DBGOUT3( << "btree_insert_nonghost_log::redo - key to insert: " << key);
    bp.insert_nonghost(key, el);
}

template <class PagePtr>
void btree_update_log::construct(
    const PagePtr   page,
    const w_keystr_t&     key,
    const char* old_el, int old_elen, const cvec_t& new_el)
{
    header._cat = 0|t_redo|t_undo|t_logical, header._type = t_btree_update;
    fill(page,
         (new (_data) btree_update_t(page->root(), key, old_el, old_elen, new_el))->size());
}

template <class PagePtr>
void btree_update_log::undo(PagePtr)
{
    btree_update_t* dp = (btree_update_t*) data();

    w_keystr_t key;
    key.construct_from_keystr(dp->_data, dp->_klen);
    vec_t old_el;
    old_el.put(dp->_data + dp->_klen, dp->_old_elen);

    // ***LOGICAL*** don't grab locks during undo
    rc_t rc = smlevel_0::bt->update_as_undo(header._stid, key, old_el);
    if(rc.is_error()) {
        W_FATAL(rc.err_num());
    }
}

template <class PagePtr>
void btree_update_log::redo(PagePtr page)
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

template <class PagePtr>
void btree_overwrite_log::construct(const PagePtr page, const w_keystr_t& key,
                                          const char* old_el, const char *new_el, size_t offset, size_t elen) {
    header._cat = 0|t_redo|t_undo|t_logical, header._type = t_btree_overwrite;
    fill(page,
         (new (_data) btree_overwrite_t(*page, key, old_el, new_el, offset, elen))->size());
}

template <class PagePtr>
void btree_overwrite_log::undo(PagePtr)
{
    btree_overwrite_t* dp = (btree_overwrite_t*) data();

    uint16_t elen = dp->_elen;
    uint16_t offset = dp->_offset;
    w_keystr_t key;
    key.construct_from_keystr(dp->_data, dp->_klen);
    const char* old_el = dp->_data + dp->_klen;

    // ***LOGICAL*** don't grab locks during undo
    rc_t rc = smlevel_0::bt->overwrite_as_undo(header._stid, key, old_el, offset, elen);
    if(rc.is_error()) {
        W_FATAL(rc.err_num());
    }
}

template <class PagePtr>
void btree_overwrite_log::redo(PagePtr page)
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

template <class PagePtr>
void btree_ghost_mark_log::construct(const PagePtr p,
                                           const vector<slotid_t>& slots,
                                           const bool is_sys_txn)
{
    header._cat = 0|t_redo|t_undo|t_logical, header._type = t_btree_ghost_mark;
    fill(p, (new (data()) btree_ghost_t<PagePtr>(p, slots, is_sys_txn))->size());
}

template <class PagePtr>
void btree_ghost_mark_log::undo(PagePtr)
{
    // UNDO of ghost marking is to get the record back to regular state
    btree_ghost_t<PagePtr>* dp = (btree_ghost_t<PagePtr>*) data();

    if (1 == dp->sys_txn)
    {
        // The insertion log record was generated by a page rebalance full logging operation
        // no 'undo' in this case
        return;
    }

    for (size_t i = 0; i < dp->cnt; ++i) {
        w_keystr_t key (dp->get_key(i));

// TODO(Restart)...
DBGOUT3( << "&&&& UNDO deletion by remove ghost mark, key: " << key);

        rc_t rc = smlevel_0::bt->undo_ghost_mark(header._stid, key);
        if(rc.is_error()) {
            cerr << " key=" << key << endl << " rc =" << rc << endl;
            W_FATAL(rc.err_num());
        }
    }
}

template <class PagePtr>
void btree_ghost_mark_log::redo(PagePtr page)
{
    // REDO is physical. mark the record as ghost again.
    w_assert1(page);
    borrowed_btree_page_h bp(page);

    w_assert1(bp.is_leaf());
    btree_ghost_t<PagePtr>* dp = (btree_ghost_t<PagePtr>*) data();

    for (size_t i = 0; i < dp->cnt; ++i) {
        w_keystr_t key (dp->get_key(i));

            // If full logging, data movement log records are generated to remove records
            // from source, we set the new fence keys for source page in page_rebalance
            // log record which happens before the data movement log records.
            // Which means the source page might contain records which will be moved
            // out after the page_rebalance log records.  Do not validate the fence keys
            // if full logging

            // Assert only if minmal logging
            w_assert2(bp.fence_contains(key));

        bool found;
        slotid_t slot;

        bp.search(key, found, slot);
        // If doing page driven REDO, page_rebalance initialized the
        // target page (foster child).
        if (!found) {
            cerr << " key=" << key << endl << " not found in btree_ghost_mark_log::redo" << endl;
            w_assert1(false); // something unexpected, but can go on.
        }
        else
        {
            // TODO(Restart)...
            DBGOUT3( << "&&&& REDO deletion, not part of full logging, key: " << key);

            bp.mark_ghost(slot);
        }
    }
}

template <class PagePtr>
void btree_ghost_reclaim_log::construct(const PagePtr p,
                                                 const vector<slotid_t>& slots)
{
    // ghost reclaim is single-log system transaction. so, use data_ssx()
    header._cat = 0|t_redo|t_single_sys_xct, header._type = t_btree_ghost_reclaim;
    fill(p, (new (data_ssx()) btree_ghost_t<PagePtr>(p, slots, false))->size());
    w_assert0(is_single_sys_xct());
}

template <class PagePtr>
void btree_ghost_reclaim_log::redo(PagePtr page)
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

template <class PagePtr>
void btree_ghost_reserve_log::construct (
    const PagePtr p, const w_keystr_t& key, int element_length) {
    // ghost creation is single-log system transaction. so, use data_ssx()
    header._cat = 0|t_redo|t_single_sys_xct, header._type = t_btree_ghost_reserve;
    fill(p, (new (data_ssx()) btree_ghost_reserve_t(key, element_length))->size());
    w_assert0(is_single_sys_xct());
}

template <class PagePtr>
void btree_ghost_reserve_log::redo(PagePtr page) {
    // REDO is to physically make the ghost record
    borrowed_btree_page_h bp(page);
    // ghost creation is single-log system transaction. so, use data_ssx()
    btree_ghost_reserve_t* dp = (btree_ghost_reserve_t*) data_ssx();

    // PHYSICAL redo.
    w_assert1(bp.is_leaf());
    bp.reserve_ghost(dp->data, dp->klen, dp->element_length);
    w_assert3(bp.is_consistent(true, true));
}

template <class PagePtr>
void btree_norec_alloc_log::construct(const PagePtr p, const PagePtr,
    PageID new_page_id, const w_keystr_t& fence, const w_keystr_t& chain_fence_high) {
    header._cat = 0|t_redo|t_multi|t_single_sys_xct, header._type = t_btree_norec_alloc;
    fill(p, (new (data_ssx()) btree_norec_alloc_t<PagePtr>(p,
        new_page_id, fence, chain_fence_high))->size());
}

template <class PagePtr>
void btree_norec_alloc_log::redo(PagePtr p) {
    w_assert1(is_single_sys_xct());
    borrowed_btree_page_h bp(p);
    btree_norec_alloc_t<PagePtr> *dp =
        reinterpret_cast<btree_norec_alloc_t<PagePtr>*>(data_ssx());

    const lsn_t &new_lsn = lsn_ck();
    w_keystr_t fence, chain_high;
    fence.construct_from_keystr(dp->_data, dp->_fence_len);
    chain_high.construct_from_keystr(dp->_data + dp->_fence_len, dp->_chain_high_len);

    PageID target_pid = p->pid();
    DBGOUT3 (<< *this << ": new_lsn=" << new_lsn
        << ", target_pid=" << target_pid << ", bp.lsn=" << bp.get_page_lsn());
    if (target_pid == dp->_page2_pid) {
        // we are recovering "page2", which is foster-child.
        w_assert0(target_pid == dp->_page2_pid);
        // This log is also a page-allocation log, so redo the page allocation.
        W_COERCE(smlevel_0::vol->alloc_a_page(dp->_page2_pid, true /* redo */));
        PageID pid = dp->_page2_pid;
        // initialize as an empty child:
        bp.format_steal(new_lsn, pid, header._stid,
                        dp->_root_pid, dp->_btree_level, 0, lsn_t::null,
                        dp->_foster_pid, dp->_foster_emlsn, fence, fence, chain_high, false);
    } else {
        // we are recovering "page", which is foster-parent.
        bp.accept_empty_child(new_lsn, dp->_page2_pid, true /*from redo*/);
    }
}

// logs for Merge/Rebalance/De-Adopt
// see jira ticket:39 "Node removal and rebalancing" (originally trac ticket:39) for detailed spec

template <class PagePtr>
void btree_foster_merge_log::construct(const PagePtr p, // destination
    const PagePtr p2,              // source
    const w_keystr_t& high,              // high (foster) of destination
    const w_keystr_t& chain_high,        // high fence of all foster nodes
    PageID foster_pid0,                 // foster page id in destination page
    lsn_t foster_pid0_emlsn,             // foster emlsn in destination page
    const int16_t prefix_len,            // source page prefix length
    const int32_t move_count,            // number of records to be moved
    const smsize_t record_data_len,      // the data length in record_data
    const cvec_t& record_data) {         // the actual data records for all the moved records,
                                         // self contained data buffer, meaning each reocrd is in the format:
                                         // ghost flag + key length + key (with sign byte) + child + ghost flag + data length + data

    header._cat = 0|t_redo|t_multi|t_single_sys_xct, header._type = t_btree_foster_merge;
    fill(p, (new (data_ssx()) btree_foster_merge_t(p2->pid(),
        high, chain_high, foster_pid0, foster_pid0_emlsn, prefix_len, move_count, record_data_len, record_data))->size());
}

template <class PagePtr>
void btree_foster_merge_log::redo(PagePtr p) {
    // See detail comments in btree_foster_rebalance_log::redo

    // Two pages are involved:
    // first page: destination, foster parent in this case
    // second page: source, foster child in this case

    // REDO is to merge it again.
    // WOD: "page" is the data source (foster child), which is written later.
    borrowed_btree_page_h bp(p);
    btree_foster_merge_t *dp = reinterpret_cast<btree_foster_merge_t*>(data_ssx());

    // WOD: "page" is the data source, which is written later.
    PageID target_pid = p->pid();

    bool recovering_dest = (target_pid == pid());   // true: recover foster parent
                                                      // false: recovery foster child
    // PageID another_pid = recovering_dest ? dp->_page2_pid : pid();
    w_assert0(recovering_dest || target_pid == dp->_page2_pid);

    if (p->is_bufferpool_managed())
    {
        // Page is buffer pool managed, so fix_direct should be safe.
        btree_page_h another;
        // CS TODO: fix_direct not currently supported
        w_assert0(false);
        // W_COERCE(another.fix_direct(another_pid, LATCH_EX));
        if (recovering_dest) {
            // we are recovering "page", which is foster-parent (dest).
            if (another.get_page_lsn() >= lsn_ck())
            {
                // If we get here, caller was not from page driven REDO phase but "page2" (src) has
                // been recovered already, this is breaking WOD rule and cannot continue

                DBGOUT3 (<< "btree_foster_merge_log::redo: caller from page driven REDO, source(foster child) has been recovered");
                W_FATAL_MSG(fcINTERNAL, << "btree_foster_merge_log::redo - WOD cannot be followed, abort the operation");
            }
            else
            {
                // thanks to WOD, "page2" (src) is also assured to be not recovered yet.
                w_assert0(another.get_page_lsn() < lsn_ck());
                btree_impl::_ux_merge_foster_apply_parent(bp /*dest*/, another /*src*/);
                W_COERCE(another.set_to_be_deleted(false));
            }
        } else {
            // we are recovering "page2", which is foster-child (src).
            // in this case, foster-parent(dest) may or may not be written yet.
            if (another.get_page_lsn() >= lsn_ck()) {
                // if page (destination) is already durable/recovered,
                // we just delete the foster child and done.
                W_COERCE(bp.set_to_be_deleted(false));
            } else {
                // dest is also old, so we are recovering both.
                btree_impl::_ux_merge_foster_apply_parent(another /*dest*/, bp /*src*/);
            }
        }
    }
    else
    {

            // Alternative and cheaper implementation without recursive
            // Extended minimal logging while the page rebalance log record is self-contained, meaning
            // it contains: new fence keys, moved record count, and the record data for all the moved records
            // therefore the target page can be recovered without involving other pages or log records
            // The same log record is used by both source and destination pages in a REDO operation:
            // Recover source page:
            //   1. No-op since the page would be dropped after the merge operation
            // Recover destination page:
            //   1. Set the low and high fence keys and the chain-high key
            //   2. Insert the moved records into page

            if (recovering_dest)
            {
                btree_page_h * dest_p = (btree_page_h*)p;

                w_keystr_t high_key, chain_high_key;

                // Get fence key information from log record (dp)
                // The _data field in log record:
                // high key + chain_high key
                // Use key lengh to offset into the _data field
                high_key.construct_from_keystr(dp->_data,        // high (foster) key is for the destination page
                                               dp->_high_len);
                chain_high_key.construct_from_keystr(dp->_data + // chain_high is the same for all foster nodes
                                               dp->_high_len, dp->_chain_high_len);

                // Set the new fence keys of the destination page (foster parent)
                // Calling format_steal to initialize the destination page (set fence keys), no stealing
                W_COERCE(dest_p->init_fence_keys(false, high_key,                // Do not reset the low fence key
                                                 true, high_key,                 // Reset the high key of the destination page
                                                 true, chain_high_key,           // Reset the chain_high
                                                 false, 0,                       // No change in destination page of non-leaf pid
                                                 false, lsn_t::null,             // No change in destination  page of non-leaf emlsn
                                                 true, dp->_foster_pid0,         // Destination  page foster pid (if any)
                                                 true, dp->_foster_pid0_emlsn)); // Destination page foster emlsn (if any)

                // Insert records into the destination page
                W_COERCE(dest_p->insert_records_dest_redo(dp->_prefix_len,
                                  dp->_move_count, dp->_record_data_len,
                                  dp->_data + dp->_high_len + dp->_chain_high_len));

                // For existing destination page (foster parent)
                // no change to the last write lsn although we inserted/moved some records
                // Update _rec_lsn only if necessary, also set the dirty flag
                // CS: no more dirty flag or rec_lsn
                // smlevel_0::bf->set_initial_rec_lsn(dest_p->pid(), dest_p->lsn(), smlevel_0::log->curr_lsn());
            }

        // Done with Single Page Recovery REDO
    }
}

template <class PagePtr>
void btree_foster_rebalance_log::construct(
    const PagePtr p,               // data destination (foster child page)
    const PagePtr p2,              // data source (foster parent page)
    const w_keystr_t& fence,             // low fence of destination, also high (foster) of source
    PageID new_pid0, lsn_t new_pid0_emlsn,
    const w_keystr_t& high,              // high (foster) of destination
    const w_keystr_t& chain_high,        // high fence of all foster nodes
    const int16_t prefix_len,            // source page prefix length
    const int32_t move_count,            // number of records to be moved
    const smsize_t record_data_len,      // the data length in record_data
    const cvec_t& record_data) {         // the actual data records for all the moved records,
                                         // self contained data buffer, meaning each reocrd is in the format:
                                         // ghost flag + key length + key (with sign byte) + child + ghost flag + data length + data

    // p - destination
    // p2 - source

    header._cat = 0|t_redo|t_multi|t_single_sys_xct, header._type = t_btree_foster_rebalance;
    fill(p, (new (data_ssx()) btree_foster_rebalance_t(p2->pid(),
        fence, new_pid0, new_pid0_emlsn, high, chain_high, prefix_len,
        move_count, record_data_len, record_data))->size());
}

template <class PagePtr>
void btree_foster_rebalance_log::redo(PagePtr p) {
    // We are relying on "page2" (src, another) to contain the pre-rebalance image,
    // therefore we can move records from 'another' (src) into 'bp' (dest).
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

    // In milestone 2:
    //    If restart_m::use_redo_full_logging_restart() is on, use solution #3 which disables
    //        the minimal logging and uses full logging instead, therefore btree_foster_rebalance_log
    //        and btree_foster_merge_log log records are used to set page fence keys only.
    //        In this code path, we will use full logging for all page rebalance and page merge
    //        operations, including both recovery REDO and normal page corruption Single-Page-Recovery
    //        operations.
    //    If restart_m::use_redo_page_restart() is on, use solution #4 which is using minimum
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
    const PageID target_pid = p->pid();
    const PageID page_id = pid();
    const PageID page2_id = dp->_page2_pid;
    const lsn_t  &redo_lsn = lsn_ck();
    DBGOUT3 (<< *this << ": redo_lsn=" << redo_lsn << ", bp.lsn=" << bp.get_page_lsn());
    w_assert1(bp.get_page_lsn() < redo_lsn);

    bool recovering_dest = (target_pid == page_id);             // 'p' is the page we are trying to recover: target
    PageID another_pid = recovering_dest ? page2_id : page_id; // another_pid: data source
                                                                // if 'p' is foster child, then another_pid is foster parent
                                                                // if 'p' is foster parent, then another_pid is foster child
                                                                // In the normal Single-Page-Recovery case with WOD,
                                                                // another_pid (foster parent)
                                                                // must be recovered after foster child

    // recovering_dest == true: recovery foster child, assume foster parent has not been recovered
    // recovering_dest == false: recover foster parent, assumed foster child has been recovered

    // CS: this assertion doesn't make sense, since recovering_dest is true if
    // and only if the two pids are equal!
    // w_assert0(recovering_dest || target_pid == page2_id);


    // Two pages are involved:
    // first page: destination, foster child in this case
    // second page: source, foster parent in this case

    // If not full logging REDO, use the optimized code path (no full logging)
    if (p->is_bufferpool_managed())
    {
        // If page is buffer pool managed (not from Recovery REDO), fix_direct should be safe.
        btree_page_h another;
        // CS TODO: fix_direct not currently supported
        w_assert0(false);
        // W_COERCE(another.fix_direct(another_pid, LATCH_EX));
        if (recovering_dest) {
            // we are recovering "page", which is foster-child (dest).
            DBGOUT1 (<< "Recovering 'page'. page2.lsn=" << another.get_page_lsn());
            if (another.get_page_lsn() >= redo_lsn)
            {
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

                w_assert0(another.get_page_lsn() < redo_lsn);
                W_COERCE(btree_impl::_ux_rebalance_foster_apply(another/*src*/, bp /*dest*/, dp->_move_count,
                                                fence, dp->_new_pid0, dp->_new_pid0_emlsn));
            }
        } else {
            // we are recovering "page2", which is foster-parent (src).
            DBGOUT1 (<< "Recovering 'page2'. page.lsn=" << another.get_page_lsn());
            if (another.get_page_lsn() >= redo_lsn) {
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
                scratch_p.format_steal(lsn_t::null, another.pid(), another.store(),
                                    bp.btree_root(), bp.level(),
                                    0, lsn_t::null, 0, lsn_t::null,
                                    high, chain_high, chain_high, false);
                W_COERCE(btree_impl::_ux_rebalance_foster_apply(bp /*src*/, scratch_p /*dest*/, dp->_move_count,
                                                fence, dp->_new_pid0, dp->_new_pid0_emlsn));
            } else {
                // dest is also old, so we are recovering both.
                W_COERCE(btree_impl::_ux_rebalance_foster_apply(bp /*src*/, another /*dest*/, dp->_move_count,
                                                fence, dp->_new_pid0, dp->_new_pid0_emlsn));
            }

        }
    }
    else
    {

        // Minimal logging while the page is not buffer pool managed, no dependency on
        // write-order-dependency

            // Alternative and cheaper implementation without recursive calls
            // Extended minimal logging while the page rebalance log record is self-contained, meaning
            // it contains: new fence keys, moved record count, and the record data for all the moved records
            // therefore the target page can be recovered without involving other pages or log records
            // The same log record is used by both source and destination pages in a REDO operation:
            // Recover source page:
            //         1. Delete the moved records from page
            //         2. Reset the high fence key accordingly, no change in chain-high key
            // Recover destination page:
            //         1. Set the low and high fence keys and the chain-high key
            //         2. Insert the moved records into page

            // Extract the fency key information first
            w_keystr_t high_key, chain_high_key;

            // Information in the log record:
            // "page" is the foster-child (destination)
            // "page2" is the foster-parent (source)
            // fence - high fence (foster) key in source and low fence key in destination after rebalance
            // high - high (foster) key in destination after rebalance
            // chain_high - high fence key for all foster nodes
            // There are changes in the pid0 and pid_emlsn of the destination page (foster child, non-leaf)
            //      the new information are in _new_pid0 and _new_pid0_emlsn
            // No information about the foster page id and foster emlsn of the destination page (foster child)
            //      because the assumption was that they have been setup in foster child page already
            // record_data_len - the total length of moved records, including all the record information
            // The actual record data is stored in '_data' field after the fence key data

            // Get fence key information from log record (dp)
            // The _data field in log record:
            // fence key + high key + chain_high key + record data for moved records
            // Use key lengh to offset into the _data field
            fence.construct_from_keystr(dp->_data, dp->_fence_len);    // fence key is the low fence of destination page
                                                                       // also the high (foster) of the source page
            high_key.construct_from_keystr(dp->_data + dp->_fence_len, // high (foster) key is for the destination page
                                           dp->_high_len);
            chain_high_key.construct_from_keystr(dp->_data + dp->_fence_len + // chain_high is the same for all foster nodes
                                           dp->_high_len, dp->_chain_high_len);

            // Get the in_doubt, dirty and dependency (starting LSN for single page recovery) information first
            w_assert1(bp.is_latched());

            // Determine whether we ar recovering source or destination page first
            if (recovering_dest)
            {
                // Recoverying destination page, which is foster child page (page 1)
                DBGOUT1 (<< "Recovering page 1 (destination page) id: " << page_id << ", source page id: " << another_pid);

                // Set the fence keys of the page which should be empty at this point
                // Calling format_steal to initialize the destination page (set fence keys), no stealing
                btree_page_h * dest_p = (btree_page_h*)p;
                W_COERCE(dest_p->format_steal(bp.get_page_lsn(), bp.pid(), bp.store(),
                                bp.btree_root(), bp.level(),
                                (bp.is_leaf())? 0 : dp->_new_pid0,                 // leaf has no pid0
                                (bp.is_leaf())? lsn_t::null : dp->_new_pid0_emlsn, // leaf has no emlsn
                                bp.get_foster(), bp.get_foster_emlsn(),    // if destination (foster child) received foster child
                                                                           // from foster parent (form a foster chain)
                                fence,            // Low fence of the destination page
                                high_key,         // High fence of the destination, valid only if three is a foster chain
                                chain_high_key,   // Chain_high_fence
                                false,            // don't log the page_img_format record
                                NULL, 0, 0,       // no steal from src_1
                                NULL, 0, 0,       // no steal from src_2
                                false,            // src_2
                                false,            // No full logging
                                false,            // No logging
                                false             // fence key record is not a ghost
                                ));

                // Insert records into the empty destination page and persist them
                W_COERCE(dest_p->insert_records_dest_redo(dp->_prefix_len, dp->_move_count,
                                  dp->_record_data_len,
                                  dp->_data + dp->_fence_len + dp->_high_len + dp->_chain_high_len));

                // For the new empty destination page (foster child)
                // the  last write lsn was set in format_steal already
                // Set the _rec_lsn using the page lsn (the last write LSN of the page), also set the dirty flag
                // smlevel_0::bf->set_initial_rec_lsn(dest_p->pid(), dest_p->lsn(), smlevel_0::log->curr_lsn());

                // Verify
                w_assert1(dp->_move_count == dest_p->nrecs());
            }
            else
            {
                // Recoverying source page, which is foster parent page (page 2)
                DBGOUT1 (<< "Recovering page 2 (source page) id: " << page2_id << ", destination page id: " << another_pid);

                // Make sure the source (foster parent) has the fost child setup already
                w_assert0(bp.get_foster() == another_pid);

                // If create a temporary destination page for the page rebalance work
                // throw away the temporary destination page after the operation
                w_keystr_t high, chain_high;
                bp.copy_fence_high_key(high);
                bp.copy_chain_fence_high_key(chain_high);
                generic_page scratch_page;
                scratch_page.tag = t_btree_p;
                btree_page_h scratch_p;
                scratch_p.fix_nonbufferpool_page(&scratch_page);
                // initialize as an empty child:

                scratch_p.format_steal(lsn_t::null, another_pid, p->store(),
                                       bp.btree_root(), bp.level(),
                                       0, lsn_t::null, 0, lsn_t::null,
                                       high, chain_high, chain_high, false);  // Do not log
                // Move records from source to destination, update fence keys
                // and reset _rec_lsn on source page (foster parent)
                W_COERCE(btree_impl::_ux_rebalance_foster_apply(bp,        // src
                                                                scratch_p, //dest
                                                                dp->_move_count,
                                                                fence, dp->_new_pid0,
                                                                dp->_new_pid0_emlsn));
            }

            // Done with the page rebalance redo, but not done with the entire recovery yet
            // _in_doubt and dependency_lsn should be set only after the page has
            // been recovered completely, therefore make sure these variables contain
            // the original values after page rebalance redo operation

            w_assert1(bp.is_latched());
        }

        // Done with Single Page Recovery REDO
}

/**
 * A \b multi-page \b SSX log record for \b btree_foster_rebalance_norec.
 * This log is totally \b self-contained, so no WOD assumed.
 */
template <class PagePtr>
void btree_foster_rebalance_norec_log::construct(
    const PagePtr p, const PagePtr, const w_keystr_t& fence) {
    header._cat = 0|t_redo|t_multi|t_single_sys_xct, header._type = t_btree_foster_rebalance_norec;
    fill(p, (new (data_ssx()) btree_foster_rebalance_norec_t(
        *p, fence))->size());
}

template <class PagePtr>
void btree_foster_rebalance_norec_log::redo(PagePtr p) {
    w_assert1(is_single_sys_xct());
    borrowed_btree_page_h bp(p);
    btree_foster_rebalance_norec_t *dp =
        reinterpret_cast<btree_foster_rebalance_norec_t*>(data_ssx());

    w_keystr_t fence, chain_high;
    fence.construct_from_keystr(dp->_data, dp->_fence_len);
    bp.copy_chain_fence_high_key(chain_high);

    PageID target_pid = p->pid();
    if (target_pid == dp->_page2_pid) {
        // we are recovering "page2", which is foster-child.
        w_assert0(target_pid == dp->_page2_pid);
        w_assert1(bp.nrecs() == 0); // this should happen only during page split.

        w_keystr_t high;
        bp.copy_fence_high_key(high);
        w_keystr_len_t prefix_len = fence.common_leading_bytes(high);
        W_COERCE(bp.replace_fence_rec_nolog_may_defrag(fence, high, chain_high, prefix_len));
    } else {
        // we are recovering "page", which is foster-parent.
        W_COERCE(bp.norecord_split(bp.get_foster(), bp.get_foster_emlsn(),
                                   fence, chain_high));
    }
}

template <class PagePtr>
void btree_foster_adopt_log::construct(const PagePtr p, const PagePtr p2,
    PageID new_child_pid, lsn_t new_child_emlsn, const w_keystr_t& new_child_key) {
    header._cat = 0|t_redo|t_multi|t_single_sys_xct, header._type = t_btree_foster_adopt;
    fill(p, (new (data_ssx()) btree_foster_adopt_t(
        p2->pid(), new_child_pid, new_child_emlsn, new_child_key))->size());
}

template <class PagePtr>
void btree_foster_adopt_log::redo(PagePtr p) {
    w_assert1(is_single_sys_xct());
    borrowed_btree_page_h bp(p);
    btree_foster_adopt_t *dp = reinterpret_cast<btree_foster_adopt_t*>(data_ssx());

    w_keystr_t new_child_key;
    new_child_key.construct_from_keystr(dp->_data, dp->_new_child_key_len);

    PageID target_pid = p->pid();
    DBGOUT3 (<< *this << " target_pid=" << target_pid << ", new_child_pid="
        << dp->_new_child_pid << ", new_child_key=" << new_child_key);
    if (target_pid == dp->_page2_pid) {
        // we are recovering "page2", which is real-child.
        w_assert0(target_pid == dp->_page2_pid);
        btree_impl::_ux_adopt_foster_apply_child(bp);
    } else {
        // we are recovering "page", which is real-parent.
        btree_impl::_ux_adopt_foster_apply_parent(bp, dp->_new_child_pid,
                                                  dp->_new_child_emlsn, new_child_key);
    }
}

template <class PagePtr>
void btree_foster_deadopt_log::construct(
    const PagePtr p, const PagePtr p2,
    PageID deadopted_pid, lsn_t deadopted_emlsn, int32_t foster_slot,
    const w_keystr_t &low, const w_keystr_t &high) {
    w_assert1(p->is_node());
    header._cat = 0|t_redo|t_multi|t_single_sys_xct, header._type = t_btree_foster_deadopt;
    fill(p, (new (data_ssx()) btree_foster_deadopt_t(p2->pid(),
        deadopted_pid, deadopted_emlsn, foster_slot, low, high))->size());
}

template <class PagePtr>
void btree_foster_deadopt_log::redo(PagePtr p) {
    // apply changes on real-parent again. no write-order dependency with foster-parent
    borrowed_btree_page_h bp(p);
    btree_foster_deadopt_t *dp = reinterpret_cast<btree_foster_deadopt_t*>(data_ssx());

    PageID target_pid = p->pid();
    if (target_pid == dp->_page2_pid) {
        // we are recovering "page2", which is foster-parent.
        w_assert0(target_pid == dp->_page2_pid);
        w_keystr_t low_key, high_key;
        low_key.construct_from_keystr(dp->_data, dp->_low_len);
        high_key.construct_from_keystr(dp->_data + dp->_low_len, dp->_high_len);
        btree_impl::_ux_deadopt_foster_apply_foster_parent(bp, dp->_deadopted_pid,
                                        dp->_deadopted_emlsn, low_key, high_key);
    } else {
        // we are recovering "page", which is real-parent.
        w_assert1(dp->_foster_slot >= 0 && dp->_foster_slot < bp.nrecs());
        btree_impl::_ux_deadopt_foster_apply_real_parent(bp, dp->_deadopted_pid,
                                                         dp->_foster_slot);
    }
}

template <class PagePtr>
void btree_split_log::construct(
        const PagePtr child_p,
        const PagePtr parent_p,
        uint16_t move_count,
        const w_keystr_t& new_high_fence,
        const w_keystr_t& new_chain
)
{
    btree_bulk_delete_t* bulk =
        new (data_ssx()) btree_bulk_delete_t(parent_p->pid(),
                    child_p->pid(), move_count,
                    new_high_fence, new_chain);
    page_img_format_t<PagePtr>* format = new (data_ssx() + bulk->size())
        page_img_format_t<PagePtr>(child_p);

    // Logrec will have the child pid as main pid (i.e., destination page).
    // Parent pid is stored in btree_bulk_delete_t, which is a
    // multi_page_log_t (i.e., source page)
    header._cat = 0|t_redo|t_multi|t_single_sys_xct, header._type = t_btree_split;
    fill(child_p, bulk->size() + format->size());
}

template <class PagePtr>
void btree_split_log::redo(PagePtr p)
{
    btree_bulk_delete_t* bulk = (btree_bulk_delete_t*) data_ssx();
    page_img_format_t<PagePtr>* format = (page_img_format_t<PagePtr>*)
        (data_ssx() + bulk->size());

    if (p->pid() == bulk->new_foster_child) {
        // redoing the foster child
        format->apply(p);
    }
    else {
        // redoing the foster parent
        borrowed_btree_page_h bp(p);
        w_assert1(bp.nrecs() > bulk->move_count);
        bp.delete_range(bp.nrecs() - bulk->move_count, bp.nrecs());

        w_keystr_t new_high_fence, new_chain;
        bulk->get_keys(new_high_fence, new_chain);

        bp.set_foster_child(bulk->new_foster_child, new_high_fence, new_chain);
    }
}

template <class PagePtr>
void btree_compress_page_log::construct(
        const PagePtr page,
        const w_keystr_t& low,
        const w_keystr_t& high,
        const w_keystr_t& chain)
{
    uint16_t low_len = low.get_length_as_keystr();
    uint16_t high_len = high.get_length_as_keystr();
    uint16_t chain_len = chain.get_length_as_keystr();

    char* ptr = data_ssx();
    memcpy(ptr, &low_len, sizeof(uint16_t));
    ptr += sizeof(uint16_t);
    memcpy(ptr, &high_len, sizeof(uint16_t));
    ptr += sizeof(uint16_t);
    memcpy(ptr, &chain_len, sizeof(uint16_t));
    ptr += sizeof(uint16_t);

    low.serialize_as_keystr(ptr);
    ptr += low_len;
    high.serialize_as_keystr(ptr);
    ptr += high_len;
    chain.serialize_as_keystr(ptr);
    ptr += chain_len;

    header._cat = 0|t_redo|t_single_sys_xct, header._type = t_btree_compress_page;
    fill(page, ptr - data_ssx());
}

template <class PagePtr>
void btree_compress_page_log::redo(PagePtr p)
{
    char* ptr = data_ssx();

    uint16_t low_len = *((uint16_t*) ptr);
    ptr += sizeof(uint16_t);
    uint16_t high_len = *((uint16_t*) ptr);
    ptr += sizeof(uint16_t);
    uint16_t chain_len = *((uint16_t*) ptr);
    ptr += sizeof(uint16_t);

    w_keystr_t low, high, chain;
    low.construct_from_keystr(ptr, low_len);
    ptr += low_len;
    high.construct_from_keystr(ptr, high_len);
    ptr += high_len;
    chain.construct_from_keystr(ptr, chain_len);

    borrowed_btree_page_h bp(p);
    bp.compress(low, high, chain, true /* redo */);
}

template void btree_norec_alloc_log::template construct<btree_page_h*>(
        btree_page_h*, btree_page_h*, PageID, const w_keystr_t&, const w_keystr_t&);

template void btree_split_log::template construct<btree_page_h*>(
        btree_page_h* child_p,
        btree_page_h* parent_p,
        uint16_t move_count,
        const w_keystr_t& new_high_fence,
        const w_keystr_t& new_chain
);

template void btree_foster_adopt_log::template construct<btree_page_h*>(
        btree_page_h* p, btree_page_h* p2,
    PageID new_child_pid, lsn_t new_child_emlsn, const w_keystr_t& new_child_key);

template void btree_insert_nonghost_log::template construct<btree_page_h*>(
    btree_page_h* page, const w_keystr_t &key, const cvec_t &el, const bool is_sys_txn);

template struct btree_ghost_t<btree_page_h*>;

template void btree_ghost_mark_log::template construct<btree_page_h*>(
        btree_page_h*, const vector<slotid_t>& slots, const bool is_sys_txn);

template void btree_insert_log::template construct<btree_page_h*>(
    btree_page_h* page,
    const w_keystr_t&   key,
    const cvec_t&       el,
    const bool          is_sys_txn);

template void btree_ghost_reclaim_log::template construct<btree_page_h*>(
        btree_page_h* p, const vector<slotid_t>& slots);

template void btree_compress_page_log::template construct<btree_page_h*>(
        btree_page_h* page,
        const w_keystr_t& low,
        const w_keystr_t& high,
        const w_keystr_t& chain);

template void btree_ghost_reserve_log::template construct<btree_page_h*>
    (btree_page_h* p, const w_keystr_t& key, int element_length);

template void btree_overwrite_log::template construct<btree_page_h*>
    (btree_page_h* page, const w_keystr_t& key,
    const char* old_el, const char *new_el, size_t offset, size_t elen) ;

template void btree_update_log::template construct<btree_page_h*>(
    btree_page_h*   page,
    const w_keystr_t&     key,
    const char* old_el, int old_elen, const cvec_t& new_el);

template void btree_foster_deadopt_log::template construct<btree_page_h*>(
    btree_page_h* p, btree_page_h* p2,
    PageID deadopted_pid, lsn_t deadopted_emlsn, int32_t foster_slot,
    const w_keystr_t &low, const w_keystr_t &high);

template void btree_foster_rebalance_log::template construct<btree_page_h*>(
    btree_page_h* p,               // data destination (foster child page)
    btree_page_h* p2,              // data source (foster parent page)
    const w_keystr_t& fence,             // low fence of destination, also high (foster) of source
    PageID new_pid0, lsn_t new_pid0_emlsn,
    const w_keystr_t& high,              // high (foster) of destination
    const w_keystr_t& chain_high,        // high fence of all foster nodes
    const int16_t prefix_len,            // source page prefix length
    const int32_t move_count,            // number of records to be moved
    const smsize_t record_data_len,      // the data length in record_data
    const cvec_t& record_data);

template void btree_foster_rebalance_norec_log::template construct<btree_page_h*>(
    btree_page_h* p, btree_page_h*, const w_keystr_t& fence);

template void btree_foster_merge_log::construct(btree_page_h* p, // destination
    btree_page_h* p2,              // source
    const w_keystr_t& high,              // high (foster) of destination
    const w_keystr_t& chain_high,        // high fence of all foster nodes
    PageID foster_pid0,                 // foster page id in destination page
    lsn_t foster_pid0_emlsn,             // foster emlsn in destination page
    const int16_t prefix_len,            // source page prefix length
    const int32_t move_count,            // number of records to be moved
    const smsize_t record_data_len,      // the data length in record_data
    const cvec_t& record_data);

template void btree_insert_log::template undo<fixable_page_h*>(fixable_page_h*);
template void btree_insert_nonghost_log::template undo<fixable_page_h*>(fixable_page_h*);
template void btree_update_log::template undo<fixable_page_h*>(fixable_page_h*);
template void btree_overwrite_log::template undo<fixable_page_h*>(fixable_page_h*);
template void btree_ghost_mark_log::template undo<fixable_page_h*>(fixable_page_h*);

template void btree_norec_alloc_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_insert_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_insert_nonghost_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_update_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_overwrite_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_ghost_mark_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_ghost_reclaim_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_ghost_reserve_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_foster_adopt_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_foster_deadopt_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_foster_merge_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_foster_rebalance_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_foster_rebalance_norec_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_split_log::template redo<btree_page_h*>(btree_page_h*);
template void btree_compress_page_log::template redo<btree_page_h*>(btree_page_h*);

template void btree_norec_alloc_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_insert_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_insert_nonghost_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_update_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_overwrite_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_ghost_mark_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_ghost_reclaim_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_ghost_reserve_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_foster_adopt_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_foster_deadopt_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_foster_merge_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_foster_rebalance_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_foster_rebalance_norec_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_split_log::template redo<fixable_page_h*>(fixable_page_h*);
template void btree_compress_page_log::template redo<fixable_page_h*>(fixable_page_h*);
