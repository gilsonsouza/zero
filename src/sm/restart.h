/*
 * (c) Copyright 2011-2014, Hewlett-Packard Development Company, LP
 */

     /*<std-header orig-src='shore' incl-file-exclusion='RESTART_H'>

        $Id: restart.h,v 1.27 2010/07/01 00:08:22 nhall Exp $

        SHORE -- Scalable Heterogeneous Object REpository

        Copyright (c) 1994-99 Computer Sciences Department, University of
                                             Wisconsin -- Madison
        All Rights Reserved.

        Permission to use, copy, modify and distribute this software and its
        documentation is hereby granted, provided that both the copyright
        notice and this permission notice appear in all copies of the
        software, derivative works or modified versions, and any portions
        thereof, and that both notices appear in supporting documentation.

        THE AUTHORS AND THE COMPUTER SCIENCES DEPARTMENT OF THE UNIVERSITY
        OF WISCONSIN - MADISON ALLOW FREE USE OF THIS SOFTWARE IN ITS
        "AS IS" CONDITION, AND THEY DISCLAIM ANY LIABILITY OF ANY KIND
        FOR ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.

        This software was developed with support by the Advanced Research
        Project Agency, ARPA order number 018 (formerly 8230), monitored by
        the U.S. Army Research Laboratory under contract DAAB07-91-C-Q518.
        Further funding for this work was provided by DARPA through
        Rome Research Laboratory Contract No. F30602-97-2-0247.

        */

#ifndef RESTART_H
#define RESTART_H

#include "w_defines.h"
#include "w_heap.h"

#include "sm_base.h"
#include "chkpt.h"
#include "lock.h"               // Lock re-acquisition

#include <map>

// Child thread created by restart_m for concurrent recovery operation
// It is to carry out the REDO and UNDO phases while the system is
// opened for user transactions
class restart_thread_t : public smthread_t
{
public:

    NORET restart_thread_t()
        : smthread_t(t_regular, "restart", WAIT_NOT_USED)
    {
        working = false;
    };
    NORET ~restart_thread_t()
    {
    };

    // Main body of the child thread
    void run();
    bool in_restart() { return working; }

private:

    bool working;

private:
    // disabled
    NORET restart_thread_t(const restart_thread_t&);
    restart_thread_t& operator=(const restart_thread_t&);
};

class restart_m
{
    friend class restart_thread_t;

public:
    restart_m(const sm_options&);
    ~restart_m();

    // Function used for concurrent operations, open system after Log Analysis
    // we need a child thread to carry out the REDO and UNDO operations
    // while concurrent user transactions are coming in
    void spawn_recovery_thread()
    {
        // CS TODO: concurrency?

        DBGOUT1(<< "Spawn child recovery thread");

        _restart_thread = new restart_thread_t;
        W_COERCE(_restart_thread->fork());
        w_assert1(_restart_thread);
    }

    void log_analysis();
    void redo_log_pass();
    void redo_page_pass();
    void undo_pass();

    chkpt_t* get_chkpt() { return &chkpt; }

private:

    // System state object, updated by log analysis
    chkpt_t chkpt;

    bool instantRestart;

    // Child thread, used only if open system after Log Analysis phase while REDO and UNDO
    // will be performed with concurrent user transactions
    restart_thread_t*           _restart_thread;

    /*
     * SINGLE-PAGE RECOVERY (SPR)
     */

    // CS: These functions were moved from log_core
    /**
    * \brief Collect relevant logs to recover the given page.
    * \ingroup Single-Page-Recovery
    * \details
    * This method starts from the log record at EMLSN and follows
    * the page-log-chain to go backward in the log file until
    * it hits a page-img log from which we can reconstruct the
    * page or it reaches the current_lsn.
    * Defined in log_spr.cpp.
    * \NOTE This method returns an error if the user had truncated
    * the transaction logs required for the recovery.
    * @param[in] pid ID of the page to recover.
    * @param[in] current_lsn the LSN the page is currently at.
    * @param[in] emlsn the LSN up to which we should recover the page.
    * @param[out] buffer into which the log records will be copied
    * @param[out] lr_pointers pointers to the individual log records within the
    * buffer
    * @pre current_lsn < emlsn
    */
    static rc_t _collect_spr_logs(
        const PageID& pid, const lsn_t &current_lsn, const lsn_t &emlsn,
        char*& buffer, list<uint32_t>& lr_offsets);

    /**
    * \brief Apply the given logs to the given page.
    * \ingroup Single-Page-Recovery
    * Defined in log_spr.cpp.
    * @param[in, out] p the page to recover.
    * @param[out] lr_pointers pointers to the individual log records to be
    * replayed, in the correct order (forward list iteration)
    * @pre p is already fixed with exclusive latch
    */
    static rc_t _apply_spr_logs(fixable_page_h &p, char* buffer,
            list<uint32_t>& lr_offsets);


public:

    /**
     * \ingroup Single-Page-Recovery
     * Defined in log_spr.cpp.
     * @copydoc ss_m::dump_page_lsn_chain(std::ostream&, const PageID &, const lsn_t&)
     */
    static void dump_page_lsn_chain(std::ostream &o, const PageID &pid, const lsn_t &max_lsn);


    /**
    * \brief Apply single-page-recovery to the given page.
    * \ingroup Single-Page-Recovery
    * Defined in log_spr.cpp.
    * \NOTE This method returns an error if the user had truncated
    * the transaction logs required for the recovery.
    * @param[in, out] p the page to recover.
    * @param[in] emlsn the LSN up to which we should recover the page
    *            (null if EMLSN not available -- must scan log to find it)
    * @param[in] from_lsn true if we can use the last write lsn on the page as
    *            the starting point for recovery, do not rely on backup file only.
    * @pre p.is_fixed() (could be bufferpool managed or non-bufferpool managed)
    */
    static rc_t recover_single_page(fixable_page_h &p, const lsn_t& emlsn);

private:
    // Function used for serialized operations, open system after the entire restart process finished
    // brief sub-routine of redo_pass() for logs that have pid.
    void                 _redo_log_with_pid(
                                logrec_t& r,                   // In: Incoming log record
                                PageID page_updated,
                                bool &redone,                  // Out: did REDO occurred?  Validation purpose
                                uint32_t &dirty_count);        // Out: dirty page count, validation purpose
};

#endif
