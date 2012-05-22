/*
 * Copyright (C) 2005-2012 Christoph Rupp (chris@crupp.de).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * See files COPYING.* for License information.
 *
 */

#include "config.h"

#include <string.h>

#include "db.h"
#include "device.h"
#include "endianswap.h"
#include "env.h"
#include "error.h"
#include "freelist.h"
#include "mem.h"
#include "btree_stats.h"
#include "txn.h"

extern ham_status_t
__freel_lazy_create32(freelist_cache_t *cache, Device *dev, Environment *env);

extern ham_status_t
__freel_destructor32(Device *dev, Environment *env);

extern ham_status_t
__freel_flush_stats32(Device *dev, Environment *env);

extern ham_status_t
__freel_mark_free32(Device *dev, Environment *env, Database *db,
                ham_offset_t address, ham_size_t size, ham_bool_t overwrite);

extern ham_status_t
__freel_check_area_is_allocated32(Device *dev, Environment *env,
                ham_offset_t address, ham_size_t size);

extern ham_status_t
__freel_alloc_area32(ham_offset_t *addr_ref, Device *dev, Environment *env,
                Database *db, ham_size_t size, ham_bool_t aligned,
                ham_offset_t lower_bound_address);

extern ham_status_t
__freel_init_perf_data32(freelist_cache_t *cache, Device *dev,
                Environment *env, freelist_entry_t *entry,
                freelist_payload_t *fp);

/**
 * replacement for env->set_dirty(); will call the macro, but also
 * add the header page to the changeset
 */
static void
__env_set_dirty(Environment *env)
{
    env->set_dirty(true);
    if (env->get_flags()&HAM_ENABLE_RECOVERY)
        env->get_changeset().add_page(env->get_header_page());
}

/**
 * replacement for the macro page->set_dirty(); will call the macro, but also
 * add the page to the changeset
 */
static void
__page_set_dirty(Page *page)
{
    Environment *env=page->get_device()->get_env();
    page->set_dirty(true);
    if (env->get_flags()&HAM_ENABLE_RECOVERY)
        env->get_changeset().add_page(page);
}

/**
 * @return the maximum number of chunks a freelist page entry can span;
 * all freelist entry pages (except the first, as it has to share the
 * db page with a (largish) database header) have this span, which is
 * a little less than
 *
 * <pre>
 * DB_CHUNKSIZE * env->get_pagesize()
 * </pre>
 */
static ham_size_t
__freel_get_freelist_entry_maxspan(Device *device, Environment *env, freelist_cache_t *cache)
{
    ham_u32_t ret;
    ham_size_t size=env->get_usable_pagesize()-db_get_freelist_header_size32();
    ham_assert((size % sizeof(ham_u64_t)) == 0, ("freelist bitarray size must be == 0 MOD sizeof(ham_u64_t) due to the scan algorithm"));
    size -= size % sizeof(ham_u64_t);

    ret = (ham_u32_t)(size*8);    /* this step is very important for pre-v1.1.0 databases as those have an integer overflow issue right here. */

    return (ham_size_t)ret;
}


static ham_status_t
__freel_cache_resize(Device *device, Environment *env, freelist_cache_t *cache,
        ham_size_t new_count)
{
    ham_size_t i;
    freelist_entry_t *entries;
    ham_size_t size_bits = __freel_get_freelist_entry_maxspan(device, env, cache);
    ham_assert(((size_bits/8) % sizeof(ham_u64_t)) == 0,
            ("freelist bitarray size must be == 0 MOD sizeof(ham_u64_t) "
             "due to the scan algorithm"));

    ham_assert(new_count > freel_cache_get_count(cache), (0));
    entries=(freelist_entry_t *)env->get_allocator()->alloc(
                    sizeof(*entries)*new_count);
    if (!entries)
        return HAM_OUT_OF_MEMORY;
    memcpy(entries, freel_cache_get_entries(cache),
            sizeof(*entries)*freel_cache_get_count(cache));

    for (i=freel_cache_get_count(cache); i<new_count; i++)
    {
        ham_status_t st;
        freelist_entry_t *entry=&entries[i];

        ham_assert(i > 0, (0));
        memset(entry, 0, sizeof(*entry));

        freel_entry_set_start_address(entry,
                freel_entry_get_start_address(&entries[i-1])+
                    freel_entry_get_max_bits(&entries[i-1])*DB_CHUNKSIZE);
        freel_entry_set_max_bits(entry, (ham_u32_t)(size_bits));

        ham_assert(cache->_init_perf_data, (0));
        st = cache->_init_perf_data(cache, device, env, entry, NULL);
        if (st)
            return st;
    }

    env->get_allocator()->free(freel_cache_get_entries(cache));
    freel_cache_set_entries(cache, entries);
    freel_cache_set_count(cache, new_count);

    return HAM_SUCCESS;
}



/**
Produce the @ref freelist_entry_t record which stores the freelist bit for the
specified @a address.

@param entry_ref call by reference; will always be set, either to NULL or a valid instance

@param dev

@param env

@param cache

@param address the specified address (a.k.a. 'RID')


@remark An important side effect of this function is that the freelist is
        extended to encompass the address space indicated by the specified storage
        device address @address.

@return HAM_SUCCESS and a valid @ref freelist_entry_t instance reference written to the
        variable pointed at by @a entry_ref

@return an error code, most probably @ref HAM_OUT_OF_MEMORY . The @a entry_ref will be
        set to NULL.

@note On exit, the following assertion will always hold:
      <pre>
      ham_assert(return_code == HAM_SUCCESS ? *entry_ref != NULL : *entry_ref == NULL, (0));
      </pre>
         i.e. the @a entry_ref will only be NULL when an error occurred.

*/
static ham_status_t
__freel_cache_get_entry(freelist_entry_t **entry_ref, Device *device, Environment *env, freelist_cache_t *cache,
        ham_offset_t address)
{
    ham_size_t i=0;
    ham_status_t st=0;
    freelist_entry_t *entries;
    
    ham_assert(entry_ref != NULL, (0));
    for(;;)
    {
        ham_size_t add;
        ham_size_t single_size_bits;

        entries=freel_cache_get_entries(cache);

        for (; i<freel_cache_get_count(cache); i++) {
            freelist_entry_t *entry=&entries[i];
    
            ham_assert(!(address<freel_entry_get_start_address(entry)),
                            (""));

            if (address>=freel_entry_get_start_address(entry)
                    && address<freel_entry_get_start_address(entry)+
                        freel_entry_get_max_bits(entry)*DB_CHUNKSIZE)
            {
                *entry_ref = entry;
                return HAM_SUCCESS;
            }
        }

        /*
         * not found? resize the table; we can guestimate where
         * 'address' is going to land within the freelist...
         */
        ham_assert(i == freel_cache_get_count(cache), (0));
        add = (ham_size_t)(address
                - freel_entry_get_start_address(&entries[i-1])
                - freel_entry_get_max_bits(&entries[i-1]));
        add += DB_CHUNKSIZE - 1;
        add /= DB_CHUNKSIZE;

        single_size_bits = __freel_get_freelist_entry_maxspan(device, env, cache);
        ham_assert(((single_size_bits/8) % sizeof(ham_u64_t)) == 0,
                ("freelist bitarray size must be == 0 MOD sizeof(ham_u64_t) "
                 "due to the scan algorithm"));

        add += single_size_bits - 1;
        add /= single_size_bits;
        ham_assert(add >= 1, (0));
        st=__freel_cache_resize(device, env, cache, i + add);
        if (st) {
            *entry_ref = 0;
            return st;
        }
        ham_assert(i<freel_cache_get_count(cache), (0));
    }
}

static ham_size_t
__freel_set_bits(Device *device, Environment *env, freelist_entry_t *entry,
        freelist_payload_t *fp, ham_bool_t overwrite,
        ham_size_t start_bit,
        ham_size_t size_bits,
        ham_bool_t set,
        freelist_hints_t *hints)
{
    ham_size_t i;
    ham_u8_t *p=freel_get_bitmap32(fp);

    ham_size_t qw_offset = start_bit & (64 - 1);
    ham_size_t qw_start = (start_bit + 64 - 1) >> 6;     /* ROUNDUP(S DIV 64) */
    ham_size_t qw_end;

    ham_assert(start_bit<freel_get_max_bits32(fp), (0));

    if (start_bit+size_bits>freel_get_max_bits32(fp))
        size_bits=freel_get_max_bits32(fp)-start_bit;

    qw_end = (start_bit + size_bits) >> 6;    /* one past the last full QWORD */

    db_update_freelist_stats_edit(device, env, entry, fp, start_bit, size_bits, set, hints);

    if (set) {
        /*
        Note: bits which are set here, may have been already SET before: this
        particular event happens when page space is freed during a transaction
        rollback, where space might or might not yet be freed, but MUST be
        ensured to be freed after a record insert failure.
        */
        if (qw_end <= qw_start)
        {
            for (i=0; i<size_bits; i++, start_bit++)
            {
                p[start_bit>>3] |= 1 << (start_bit&(8-1));
            }
        }
        else
        {
            ham_size_t n = size_bits;
            ham_u64_t *p64=(ham_u64_t *)freel_get_bitmap32(fp);
            p64 += qw_start;
            
            if (qw_offset)
            {
                p = (ham_u8_t *)&p64[-1];

                for (i = qw_offset; i < 64; i++)
                {
                    p[i>>3] |= 1 << (i&(8-1));
                }
                
                n -= 64 - qw_offset;
            }

            qw_end -= qw_start;
            for (i = 0; i < qw_end; i++)
            {
                p64[i] = 0xFFFFFFFFFFFFFFFFULL;
            }

            p = (ham_u8_t *)&p64[qw_end];

            n -= qw_end << 6;

            for (i = 0; i < n; i++)
            {
                p[i>>3] |= 1 << (i&(8-1));
            }
        }
    }
    else {
        if (qw_end <= qw_start)
        {
            for (i=0; i<size_bits; i++, start_bit++)
            {
                p[start_bit>>3] &= ~(1 << (start_bit&(8-1)));
            }
        }
        else
        {
            ham_size_t n = size_bits;
            ham_u64_t *p64=(ham_u64_t *)freel_get_bitmap32(fp);
            p64 += qw_start;

            if (qw_offset)
            {
                p = (ham_u8_t *)&p64[-1];

                for (i = qw_offset; i < 64; i++)
                {
                    p[i>>3] &= ~(1 << (i&(8-1)));
                }

                n -= 64 - qw_offset;
            }

            qw_end -= qw_start;
            for (i = 0; i < qw_end; i++)
            {
                p64[i] = 0;
            }

            p = (ham_u8_t *)&p64[qw_end];

            n -= qw_end << 6;

            for (i = 0; i < n; i++)
            {
                p[i>>3] &= ~(1 << (i&(8-1)));
            }
        }
    }

    return size_bits;
}

/**
* Search for a sufficiently large free slot in the freelist bit-array.
*
* Before v1.0.9, this was a sequential scan, sped up by first scanning
* QWORDs in an outer loop in order to find spots with at least 1 free
* bit, then an inner loop which would perform a bit-level scan only a
* free bit was located by the outer loop.
*
* The 'aligned' search acted a little different: it had an outer loop
* which scanned BYTEs at a time, instead of QWORDs, while the inner
* bit-level scan loop would only last until the requested number of
* bits had been scanned, when failing to hit a valid free slot, thus
* returning to the outer, faster loop (a behaviour which was NOT
* exhibited by the 'regular' search method: once inside the inner
* bit-level loop, it would _stay_ there. The 'aligned' scan would also
* stop scanning when the end-requested_size bit was tested, while the
* 'regular' loop continued on until the very end of the bitarray.
*
* This was very slow, especially in scenarios where tiny free slots
* are located near the front of the bitarray (which represents the
* storage layout of the whole database, incidentally).
*
*
*  A few improvements can be thought of (and have been implemented):
*
* - first off, do as the 'aligned' search already did, but now for
*   everyone: stop scanning at the END-requested_size bit: any
*   free space _starting_ beyond that point is too small anyway.
*
* - 'aligned' search is searching for space aligned at a DB page
*   edge (256 bytes or bigger) and since we 'know' the requested size
*   is also large (and very, very probably a multiple of
*   64*DB_CHUNKSIZE (== 2K) as the only one requesting
*   page-aligned storage is requesting an entire page (>=2K!) of
*   storage anyway), we can get away here by NOT scanning at
*   bit level, but at QWORD level only.
*
*   EDIT v1.1.1: This has been augmented by a BYTE-level search as
*                odd-Kb pagesizes do exist (e.g. 1K pages) and these
*                are NOT aligned to the QWORD boundary of
*                <code>64 * DB_CHUNKSIZE</code> = 2Kbytes (this is the
*                amount of storage space covered by a single QWORD
*                worth of freelist bits).  See also the @ref DB_PAGESIZE_MIN_REQD_ALIGNMENT define.
*
* - Boyer-Moore scanning instead of sequential: since a search
*   for free space is basically a search for a series of
*   SIZE '1' bits, we can employ characteristics as used by the
*   Boyer-Moore string search algorithm (and it's later
*   improvements, such as described by [Hume & Sunday]). While we
*   'suffer' from the fact that we are looking for 'string
*   matches' in an array which has a character alphabet of size 2:
*   {0, 1}, as we are considering BITS here, we can still employ
*   the ideas of Boyer-Moore et al to speed up our search
*   significantly. Here are several elements to consider:
*
*   # we can not easily (or not at all) implement the
*     suggested improvement where there's a sentinel at the
*     end of the searched range, as we are accessing mapped
*     memory, which will cause an 'illegal access' exception
*     to fire when we sample bytes/words outside the alloted
*     range. Of course, this issue could be resolved by
*     'tweaking' the freelist pages upon creation by ensuring
*     there's a 'sentinel range' available at the end of each
*     freelist page. THAT will be something to consider for
*     the 'modern' Data Access Mode freelist algorithm(s)...
*
*   # since we have an alphabet of size 2, we don't have to
*     bother with 'least frequent' and 'most frequent'
*     characters in our pattern: we will _always_ be looking
*     for a series of 1 bits. However, we can improve the scan,
*     as was done in the classic search algorithm, by inspecting
*     QWORDs at a time instead of bits. Still, we can think of
*     the alphabet as being size = 2, as there's just two
*     character values of interest: 0 and 'anything else', which
*     is our '1' in there. Expressing the length of the searched
*     pattern in QWORDs will also help find probable slots
*     as we can stick to the QWORD-dimensioned scanning as
*     long as possible, only resolving to bit-level scans at
*     the 'edges' of the pattern.
*
*   # the classic BM (Boyer-Moore) search inspected the character
*     at the end of the pattern and then backtracked; we can
*     improve our backtracking by assuming a few things about
*     both the pattern and the search space: since our pattern
*     is all-1s and we can can assume that our search space,
*     delimited by a previous sample which was false, and the
*     latest sample, distanced pattern_length bits apart, is
*     mostly 'used bits' (zeroes), we MAY assume that the
*     free space in there is available more towards the end
*     of this piece of the range. In other words: the searched
*     space can be assumed to be SORTED over the current
*     pattern_length bitrange -- which means we can employ
*     a binary search mechanism to find the 'lowest' 1-bit
*     in there. We add an average cost there of the binary
*     search at O(log(P)) (where P = pattern_size) as we
*     will have to validate the result returned by such a
*     binary search by scanning forward sequentially, but
*     on average, we will save cycles as we do the same
*     bsearch on the NEXT chunk of size P, where we assume
*     the data is sorted in REVERSE order and look for the
*     first '0' instead: these two bsearches will quickly
*     deliver a sufficiently trustworthy 'probable size of
*     free area' to do this before we wind down to a (costly)
*     sequential scan. Note that the two bsearches can
*     be reduced to the first only, if its verdict is that
*     the range starts at offset -P+1, i.e. the first bit
*     past the previous (failed) sample in the skip loop.
*     The two blocks bsearched are, given the above, assumed
*     to show a series of '1' bits within an outer zone
*     of '0' bits on both sides; that's why the second
*     bsearch should assume REVERSE sorted order, as we wish
*     to find the first '0' AFTER the last '1' in there,
*     so that we have an indicator of the end-of-1-range
*     position in the search space.
*
*   # as we look for an all-1 pattern, our skip loop can
*     skip P-1 bits at a time, as a bit sampled being '0'
*     means the P'th bit after that one must be '1' to
*     get us a match. When we get such a hit, we do not
*     know if it's the start or end of the match yet,
*     so that's why we scan backwards and forwards using
*     the bsearches suggested above. (Especially for large
*     pattern sizes is the bsearch-before-sequential 'prescan'
*     considered beneficial.)
*
*   # As we scan the freelist, we can gather statictics:
*     how far we had to scan into the entire range before
*     we hit our _real_ free slot:
*     by remembering this position, the next search for
*     a similar sized pattern can be sped up by starting
*     at the position (adjusted: + old P size, of course)
*     we found our last match.
*
*     When we delete a record, we can adjust this position
*     to the newly created free space, when the deleted
*     entry creates a suitably large free area.
*
*     This implies that we might want to keep track of
*     a 'search start position' for a set of sizes instead
*     of just one: even on a fixed-width DB, there's the
*     key and the record data. The initial idea here is
*     to track it for log8(P) ranges, i.e. one tracker
*     for sizes up to 2^8, one more for sizes up 2^16,
*     nd so on (maybe later upgrade this to log2(P) ranges).
*
*   # As we scan the freelist, we can gather statictics:
*     the number of times we had a 'probable hit' (which
*     failed to deliver):
*     As the ratio of the number of 'false hits' versus
*     actual searches increases, we can speed up our
*     searches by looking for a larger free slot (maybe
*     even using the first-pos tracker for the next larger
*     sizes set as mentioned in the previous point):
*     by doing so we can, hopefully, start at a higher
*     position within the range. At the cost of creating
*     'gaps' in the storage which will remain unused for
*     a long time (in our current model, these statistics
*     are gathered per run, so the next open/access/close
*     run of the DB will reset these statistics).
*
* Further notes:
*
* as we keep the statistics in cache
* rather permanently (as long as the cache itself lives),
* any changes applied to the DB freelist by a second,
* asynchronous writer (freeing additional space in
* the freelist there) will go undiscovered, at least as
* far a extra FREEd space is concerned; changes which
* ALLOCATE space will be detected immediately as the
* freelist data is scanned.
* The concequence is a probably larger DB file and more
* freelist fragmentation when multiple writers access
* a single DB -- which I frown upon anyway.
*
*
* The Boyer-Moore skip loop can help us jump through
* the freelist pages faster; this skip loop can be
* employed at both the QWORD and BIT search levels.
*
*   # The bsearch backtracking 'prescans' should maybe
*     be disabled for smaller sizes, e.g. for sizes up
*     to length = 8, as it does not help speed up
*     matters a whole darn lot in that case anyway.
*
*   # An alternative to plain Boyer-Moore skip loop, etc.
*     is to take the bsearch idea a step further: we
*     know the skip loop step size (P), given the
*     pattern we are looking for.
*
*     We may also assume that most free space is located
*     at the end of the range: when we express that
*     free space available anywhere in the freelist
*     'but at the very end' is less valuable, we can
*     assume the freelist is SORTED: by not starting
*     by a sequential skip loop scan, but using a
*     bsearch to find the lowest available '1'
*     probable match, we can further improve upon
*     the concept of 'starting at the last known offset'
*     as suggested above. This means we can start
*     the search by a binary search of the range
*     [last_offset .. end_of_freelist] to find the
*     first probable sample match, after which we
*     can go forward using your regular Boyer-Moore
*     skip loop.
*
*     This will [probably] lose free '1' slots which
*     sit within larger '0' areas, but that's what
*     this is about. When our DB access behaviour is
*     generally a lot of insert() and little or
*     no delete(), we can use this approach to get
*     us some free space faster.
*
*   # The above can be enhanced even further by
*     letting HamsterDB gather our access statistics
*     (~ count the number of inserts and deletes
*     during a run) to arrive at an automated choice
*     for this mechanism over others available;
*     instead of the user having to specify a
*     preferred/assumed Data Access Mode, we can
*     deduce the actual one ourselves.
*
*     The drawback of this bsearch-based free slot
*     searching is that we will not re-use free
*     slots within the currently oocupied space,
*     i.e. more freelist fragmentation and a larger
*     DB file as a aresult.
*
*   # Note however that the 'start off with a
*     range bsearch' is internally different from
*     the one/two bsearches in the space backtrack
*     'prescan':
*
*     the latter divide up inspected
*     space to slices of 1 bit each, unless we
*     limit the bsearch prescan to BYTE-level,
*     i.e. 8-bit slices only for speed sake.
*     AH! ANOTHER IMPROVEMENT THERE!
*
*     the former (bsearch-at-start) will ALWAYS
*     limit its divide-and-conquer to slices of
*     P bits (or more); further reducing the
*     minimum slice is identical to having a BM
*     skip loop with a jump distance of P/2 (or
*     lower), which is considered sub-optimal.
*     Such a bsearch would be blending the search
*     pattern into the task area alotted the
*     dual-bsearch backtrack prescans.
*
*     Another notable difference is that the
*     backtracking/forward-tracking inner bsearch
*     prescans can act differently on the
*     discovery of an apparently UNORDERED
*     search space: those bsearches may hit '0's
*     within a zone of '1's, i.e. hit the '0' marked
*     '^' in this search space - which was assumed
*     to be ORDERED but clearly is NOT:
*
*       0000 1111 1111 0^111
*
*     and such an occurrence (previous lower
*     sample=='1', while current sample==='0')
*     can cause those bsearches to stop scanning
*     this division and immediate adjust the range
*     to current_pos+1..end_of_range and continue
*     to sample the median of that new range.
*     This would be absolutely valid bahaviour.
*
*     (Reverse '0' and '1' and range determination
*      for the second, forward-tracking bsearch
*      there, BTW.)
*
*     However, the starting, i.e. 'outer' bsearch
*     may not decide to act that way: after all,
*     the range may have gaps, one of which
*     has just been discovered, so here the bsearch
*     should really assume the newly found in-zone
*     '1' free marker to be at the END of the
*     inspected range and look for more '1's down
*     from here: after all, this bsearch is looking
*     for the first PROBABLE free slot and as such
*     is a close relative of the BM skip loop.
*
*   # as our pattern is all-1s anyway, there is
*     problem in adjusted the BM search so as
*     to assume we're skiploop-scanning for the
*     FIRST character in the pattern; after all,
*     it's identical to the LAST one: '1'.
*
*     This implies that we have simpler code while
*     dealing with aligned searches as well as
*     regular. And no matter if our skip-search was
*     meant to look for the last (or first)
*     character: any hit would mean we've hit a
*     spot somewhere 'in the middle' of the search
*     pattern; given the all-1s, we then need to
*     find out through backtracking (and forward~)
*     where in the pattern we did land: at the
*     start, end or really in the middle.
*
*     Meanwhile, aligned matches are kept simple
*     this way, as they now can assume that they
*     always landed at the START of the pattern.
*
* --------
*
* FURTHER THOUGHTS:
*
*   # given our initial implementation and analysis,
*     we can assume that the 'header page' is always
*     reserved in the freelist for any valid database.
*
*     This is a major important bit of info, as it
*     essentially serves as both a sentinel, which
*     has a pagesize, i.e. is a sentinel as large
*     as the largest freelist request (as those
*     come in one page or smaller at a time).
*
*     This gives us the chance to implement other
*     Boyer-Moore optimizations: we don't need
*     to check the lower bound any longer AND
*     we can always start each scan at START+PAGE
*     offset at least, thus skipping those headerpage
*     '0' bits each time during the regular phase of
*     each search.
*
*     [Edit] Unfortunately, this fact only applies
*     to the initial freelist page, so we cannot use
*     it as suggested above :-(
*
*   # aligned scans are START-probe based, while
*     unaligned scans use the classic Boyer-Moore
*     END-probe; this is faster overall, as the subsequent
*     REV linear scan will then produce the length
*     of the leading range, which is (a) often
*     enough to resolve the request, and (b) is
*     hugging previous allocations when we're
*     scanning at the end of the search space,
*     which is an desirable artifact.
*
*     This does not remove the need for some
*     optional FWD linear scans to determine the
*     suitability of the local range, but these
*     will happen less often.
*
* @author Ger Hobbelt, ger@hobbelt.com
*/

/** 8 QWORDS or less: 1-stage scan, otherwise, bsearch pre-scan */
#define SIMPLE_SCAN_THRESHOLD            8




/**
 * adjust the bit index to the lowest MSBit which is part of a
 * consecutive '1' series starting at the top of the QWORD
 */
static __inline ham_u32_t
BITSCAN_MSBit(ham_u64_t v, ham_u32_t pos)
{
    register ham_s64_t value = (ham_s64_t)v;

    /*
     * test top bit by checking two's complement sign.
     *
     * This is crafted to spend the least number of
     * rounds inside the BM freelist bitarray scans.
     */
    while (value < 0)
    {
        pos--;
        value <<= 1;
    }
    return pos;
}

static __inline ham_u32_t
BITSCAN_MSBit8(ham_u8_t v, ham_u32_t pos)
{
    register ham_s8_t value = (ham_s8_t)v;

    /*
     * test top bit by checking two's complement sign.
     *
     * This is crafted to spend the least number of
     * rounds inside the BM freelist bitarray scans.
     */
    while (value < 0)
    {
        pos--;
        value <<= 1;
    }
    return pos;
}

/**
 * adjust the bit index to <em> 1 PAST </em> the highest LSBit which is
 * part of a consecutive '1' series starting at the bottom of the QWORD.
 */
static __inline ham_u32_t
BITSCAN_LSBit(ham_u64_t v, ham_u32_t pos)
{
    register ham_u64_t value = v;

    /*
     * test bottom bit.
     *
     * This is crafted to spend the least number of
     * rounds inside the BM freelist bitarray scans.
     */
    while (value & 0x01)
    {
        pos++;
        value >>= 1;
    }
    return pos;
}

static __inline ham_u32_t
BITSCAN_LSBit8(ham_u8_t v, ham_u32_t pos)
{
    register ham_u8_t value = v;

    /*
     * test bottom bit.
     *
     * This is crafted to spend the least number of
     * rounds inside the BM freelist bitarray scans.
     */
    while (value & 0x01)
    {
        pos++;
        value >>= 1;
    }
    return pos;
}


static ham_s32_t
__freel_search_bits_ex(Device *device, Environment *env,
                freelist_entry_t *entry, freelist_payload_t *f,
                ham_size_t size_bits, freelist_hints_t *hints)
{
    ham_u32_t end;
    ham_u64_t *p64=(ham_u64_t *)freel_get_bitmap32(f);
    ham_u32_t start;
    ham_u32_t min_slice_width;

    ham_assert(hints->cost == 1, (0));
    start = hints->startpos;
    end = hints->endpos;
    min_slice_width = hints->skip_distance;

    /* as freelist pages are created, they should span a multiple of
     * 64(=QWORD bits) DB_CHUNKS! */
    ham_assert(end <= freel_get_max_bits32(f), (0));
    ham_assert(freel_get_max_bits32(f) % 64 == 0, (0));

    /* sanity checks */
    ham_assert(end > start, (0));
    ham_assert(min_slice_width > 0, (0));
    ham_assert(freel_get_max_bits32(f) >= freel_get_allocated_bits32(f), (0));

    /*
     * start-of-scan speedups:
     *
     * 1) freelist pages are created and then filled with zeroes,
     * EXCEPT for those slots which have an actual disc page related to
     * them. Hence, maxbits is a bit of a lie, really: only when a page
     * has 'overflow' can we expect a freelist to be entirely occupied.
     *
     * Hence we can speed up matters a bit by quick-scanning for the
     * end of the occupied zone: from the end of the freelist we descend
     * by pagesize/CHUNK steps probing for free slots. A special case:
     * when none are found, the total range is still assumed to be the
     * entire freelist page, in order to prevent permanent gaps which
     * will never be filled. Of course, this choice is mode-dependent: in
     * higher modes, we care less about those gaps.
     *
     * 2) we can inspect the 'allocated_bits' count (which decreases as
     * bits are occupied) - this value tells us something about the
     * total number of available free slots. We can discard the chance
     * of any luck finding a suitable slot for any requests which are
     * larger than this number.
     */

    ham_assert(size_bits <= freel_get_max_bits32(f), (0));

    /* #2 */
    ham_assert(size_bits <= freel_entry_get_allocated_bits(entry), (0));
    ham_assert(size_bits <= freel_get_allocated_bits32(f), (0));

    /* #3: get a hint where to start searching for free space: DONE ALREADY */

    /*
     * make sure the starting point is a valid sample spot. Also, it's
     * no use to go looking when we won't have a chance for a hit
     * anyway.
     */
    if (start + size_bits > end) {
        db_update_freelist_stats_fail(device, env, entry, f, hints);
        return (-1);
    }

    /* determine the first aligned starting point: */
    if (hints->aligned)
    {
        ham_u32_t chunked_pagesize = env->get_pagesize() / DB_CHUNKSIZE;
        ham_u32_t offset = (ham_u32_t)(freel_get_start_address(f) / DB_CHUNKSIZE);
        offset %= chunked_pagesize;
        offset = chunked_pagesize - offset;
        offset %= chunked_pagesize;

        /*
         * now calculate the aligned start position
         *
         * as freelist pages are created, they should span a multiple
         * of 64 DB_CHUNKS!
         */
        if (start < offset)
        {
            start = offset;
        }
        else
        {
            start -= offset;
            start += chunked_pagesize - 1;
            start -= start % chunked_pagesize;
            start += offset;
        }

        /*
         * align 'end' as well: no use scanning further than that one.
         * (This of course assumes a free page-aligned slot is available
         * ENTIRELY WITHIN the bitspace carried by a single freelist
         * page; alas, there're enough of those, and the ones, if any,
         * crossing over the freelist page boundary, are welcome to to
         * the other free slot searches coming in. ;-)
         *
         * Of course, this also assumes any 'aligned' (or any other!)
         * request for a free zone all are small enough to span only a
         * single freelist page. This is okay; huge blobs are the only
         * possible exception and as far as I gathered those are handled
         * on a page-at-a-time basis anyway, reducing them to multiple
         * 'unrelated' pagesized free zone queries to us here.
         *
         * Note that freelist pages do NOT have to start their bitarray
         * at a pagesize-aligned address, at least not theoretically. We
         * resolve this here by aligning the 'end' by first converting
         * it to a fake address of sorts by subtracting 'offset'. When
         * we have done that, we can align it to a page boundary like
         * everybody else (EXCEPT we need to round DOWN here as we are
         * looking at an END marker instead of a START marker!) and when
         * that's done as well, we shift 'end' back up by offset,
         * putting it back where it should be.
         */
        ham_assert(end >= offset, (0));
        end -= offset;
        end -= end % chunked_pagesize; /* round DOWN to boundary */
        end += offset;

        /*
         * adjust minimum step size also: it's no use scanning the
         * non-aligned spots after all
         */
        min_slice_width += chunked_pagesize - 1;
        min_slice_width -= min_slice_width % chunked_pagesize;

        /*
         * make sure the starting point is a valid sample spot:
         * since we aligned start & end, they may now be identical:
         * no space here then...
         */
        ham_assert(start <= end, (0));
        /*
         * Also, it's no use to go looking when we won't have a chance
         * for a hit anyway.
         */
        if (start + size_bits > end)
        {
            db_update_freelist_stats_fail(device, env, entry, f, hints);
            return (-1);
        }
    }
    ham_assert(start < end, (0));

    /*
     * in order to cut down on the number of overlapping tests, we
     * skip-loop scan for the first probable hit.
     *
     * This way we ensure that, as soon as we've left this mode-switch
     * section and enter the big BM-loop below, our 'start' already
     * points at a probable hit at all times!
     *
     *
     * sequential scan: the usual BM skip loop, with a twist:
     *
     * when the size we're looking for is large enough, we know we need
     * 1 or more all-1s qwords and we search for those then.
     *
     * At least one all-1s QWORD is required when the
     * requested space is >= 2 QWORDS:
     *
     * e.g. layout '0001 1111 1110'
     *
     * and as 'min_slice_width' is a rounded-up value, we'd better
     * check with the original: 'size_bits'
     */
    if (hints->aligned)
    {
        if (start % 64 == 0 && end % 64 == 0)
        {
            /*
             * The alignment is a QWORD(64)*CHUNKSIZE(32) multiple (= 2K),
             * so we'll be able to scan the freelist using QWORDs only,
             * which is fastest.
             */

            /* probing START positions; bm_l is the "left" start offset
             * in the bitmap. bm_r is the EXCLUSIVE upper bound */
            ham_u32_t bm_l = start / 64;
            ham_u32_t min_slice_width64 = (min_slice_width + 64 - 1) / 64;
            ham_u32_t bm_r = end / 64 - min_slice_width64 + 1;

            /*
             * we know which start positions are viable; we only
             * inspect those.
             *
             * Besides, we assume ALIGNED searches require 1 all-1s
             * qword at least; this improves our skipscan here.
             */
            while (bm_l < bm_r) {
                hints->cost++;

                if (p64[bm_l] == 0xFFFFFFFFFFFFFFFFULL)
                {
                    /*
                     * BM: a hit: see if we have a sufficiently large
                     * free zone here
                     */
                    break;
                }

                bm_l += min_slice_width64;
            }

            /* report our failure to find a free slot */
            if (bm_l >= bm_r) {
                db_update_freelist_stats_fail(device, env, entry, f, hints);
                return (-1);
            }

            /* BM search with a startup twist already done */
            for (;;)
            {
                ham_assert(p64[bm_l] == 0xFFFFFFFFFFFFFFFFULL, (0));

                hints->cost++;

                /*
                 * We also assume such aligned scans require all-1s qwords
                 * EXCLUSIVELY, i.e. no dangling bit tail for these, my
                 * friend. Just all-1s qwords all the way.
                 *
                 * we already know we're at the STARTING spot of this one:
                 * in our case it's just a forward scan, maybe with a
                 * little guard check, is all we're gonna need.
                 *
                 * However, since we happen to know the SIZE we're looking
                 * for is rather large, we perform a PRE-SCAN by binary
                 * searching the forward
                 * range (no need to scan backwards: we've been there in a
                 * previous round if there was anything interesting in
                 * there).
                 *
                 * To help the multi-level guard check succeed, we have to
                 * assume a few things:
                 *
                 * we know the START. It is fixed. So all we need to do is
                 * to find a '0' bit in the pre-scan of the SIZE range and
                 * we can be assured the current zone is toast.
                 *
                 * We assume in this locality: the '0' bit in there is most
                 * probably located near the start of the range, if any.
                 *
                 * the guard check only remotely looks like a bsearch: it
                 * starts at START and then divides the space in 2 on every
                 * round, until the END marker is hit. Any '0' bit in the
                 * inspected qwords will trigger a FAIL for this zone.
                 */
                if (min_slice_width64 > SIMPLE_SCAN_THRESHOLD)
                {
                    ham_u32_t l = bm_l + 1; /* START qword is already checked */
                    ham_u32_t r = l + min_slice_width64 - 1; /* EXCLUSIVE upper bound */
                    while (l < r)
                    {
                        hints->cost++;

                        if (p64[l] != 0xFFFFFFFFFFFFFFFFULL)
                            break;
                        /* make sure we get at l==r at some point: */
                        l = (l + r + 1) / 2;
                    }
                    if (l == r)
                    {
                        /*
                         * all guard checks have passed.
                         *
                         * WARNING: note that due to the way the guard
                         * check loop was coded, we are now SURE the initial
                         * QWORD
                         * _and_ last QWORD are all-1s at least, so we
                         * don't have to linear-scan those again.
                         */

                        /* linear forward validation scan */
                        r--; /* topmost all-1s qword of the acceptable range + 1 */
                        l = bm_l + 1; /* skip first qword */

                        for ( ; l < r; l++)
                        {
                            hints->cost++;

                            if (p64[l] != 0xFFFFFFFFFFFFFFFFULL)
                                break;
                        }
                        if (r == l)
                        {
                            /* a perfect hit: report this one as a match! */
                            db_update_freelist_stats(device, env, entry, f, bm_l * 64, hints);
                            return bm_l * 64;
                        }
                    }
                }
                else
                {
                    /*
                     * simple scan only: tiny range
                     *
                     * Nevertheless, we also have checked our first QWORD,
                     * so we can skip that one
                     */
                    ham_u32_t l = bm_l + 1; /* we have checked the START qword already */
                    ham_u32_t r = l + min_slice_width64 - 1; /* EXCLUSIVE upper bound */

                    /* linear forward validation scan */
                    for ( ; l < r; l++)
                    {
                        hints->cost++;

                        if (p64[l] != 0xFFFFFFFFFFFFFFFFULL)
                            break;
                    }
                    if (r == l)
                    {
                        /* a perfect hit: report this one as a match! */
                        db_update_freelist_stats(device, env, entry, f, bm_l * 64, hints);
                        return bm_l * 64;
                    }
                }

                /*
                 * when we get here, we've failed the inner sequence
                 * validation of an aligned search; all we can do now is try
                 * again at the next aligned scan location.
                 *
                 * This is the simplest post-backtrack skip of the bunch,
                 * Sunday/Hume-wise, but nothing improves upon this (unless
                 * we were scanning a size span in there which would've been
                 * larger than the skip step here, and that NEVER happens
                 * thanks to our prep work at the start of this function.
                 */
                bm_l += min_slice_width64;

                /*
                 * we know which start positions are viable; we only
                 * inspect those.
                 *
                 * Besides, we assume ALIGNED searches require 1 all-1s
                 * qword at least; this improves our skipscan here.
                 */
                while (bm_l < bm_r) {
                    hints->cost++;

                    if (p64[bm_l] == 0xFFFFFFFFFFFFFFFFULL)
                    {
                        /*
                         * BM: a hit: see if we have a sufficiently large
                         * free zone here
                         */
                        break;
                    }

                    /* BM: a miss: skip to next opportunity sequentially */
                    bm_l += min_slice_width64;
                }

                if (bm_l >= bm_r) {
                    /* report our failure to find a free slot */
                    db_update_freelist_stats_fail(device, env, entry, f, hints);
                    return (-1);
                }
            }
        }
        else
        {
/*TODO is this code ever reached? or only with 1kb pages? */
            /*
             * The alignment is NOT a QWORD(64)*CHUNKSIZE(32) multiple (= 2K),
             * so we'll have to contend ourselves with a BYTE-based scan
             * instead, which would mean our minimum allowed alignment would be
             * BYTE(8)*CHUNKSIZE(32) == 256 bytes alignment.
             */
            ham_u8_t *p8=(ham_u8_t *)p64;

            /* probing START positions */
            ham_u32_t bm_l = start / 8;
            ham_u32_t min_slice_width8 = (min_slice_width + 8 - 1) / 8;
            ham_u32_t bm_r = end / 8 - min_slice_width8 + 1; /* EXCLUSIVE upper bound */

            /*
            * we know which start positions are viable; we only
            * inspect those.
            *
            * Besides, we assume ALIGNED searches require 1 all-1s
            * byte at least; this improves our skipscan here.
            */
            while (bm_l < bm_r) {
                hints->cost++;

                if (p8[bm_l] == 0xFFU)
                {
                    /*
                    * BM: a hit: see if we have a sufficiently large
                    * free zone here
                    */
                    break;
                }

                /* BM: a miss: skip to next opportunity sequentially */
                bm_l += min_slice_width8;
            }

            if (bm_l >= bm_r) {
                /* report our failure to find a free slot */
                db_update_freelist_stats_fail(device, env, entry, f, hints);
                return (-1);
            }

            /* BM search with a startup twist already done */
            for (;;)
            {
                ham_assert(p8[bm_l] == 0xFFU, (0));

                hints->cost++;

                /*
                * We also assume such aligned scans require all-1s bytes
                * EXCLUSIVELY, i.e. no dangling bit tail for these, my
                * friend. Just all-1s bytes all the way.
                *
                * we already know we're at the STARTING spot of this one:
                * in our case it's just a forward scan, maybe with a
                * little guard check, is all we're gonna need.
                *
                * However, since we happen to know the SIZE we're looking
                * for is rather large, we perform a PRE-SCAN by binary
                * searching the forward
                * range (no need to scan backwards: we've been there in a
                * previous round if there was anything interesting in
                * there).
                *
                * To help the multi-level guard check succeed, we have to
                * assume a few things:
                *
                * we know the START. It is fixed. So all we need to do is
                * to find a '0' bit in the pre-scan of the SIZE range and
                * we can be assured the current zone is toast.
                *
                * We assume in this locality: the '0' bit in there is most
                * probably located near the start of the range, if any.
                *
                * the guard check only remotely looks like a bsearch: it
                * starts at START and then divides the space in 2 on every
                * round, until the END marker is hit. Any '0' bit in the
                * inspected bytes will trigger a FAIL for this zone.
                */
                if (min_slice_width8 > SIMPLE_SCAN_THRESHOLD)
                {
                    ham_u32_t l = bm_l + 1; /* we have checked the START byte already */
                    ham_u32_t r = l + min_slice_width8 - 1; /* EXCLUSIVE upper bound */
                    while (l < r)
                    {
                        hints->cost++;

                        if (p8[l] != 0xFFU)
                            break;
                        /* make sure we get at l==r at some point: */
                        l = (l + r + 1) / 2;
                    }
                    if (l == r)
                    {
                        /*
                        * all guard checks have passed.
                        *
                        * WARNING: note that due to the way the guard
                        * check loop was coded, we are now SURE the initial
                        * BYTE
                        * _and_ last BYTE are all-1s at least, so we
                        * don't have to linear-scan those again.
                        */

                        /* linear forward validation scan */
                        r--; /* topmost all-1s byte of the acceptable range + 1 */
                        l = bm_l + 1; /* skip first byte */

                        for ( ; l < r; l++)
                        {
                            hints->cost++;

                            if (p8[l] != 0xFFU)
                                break;
                        }
                        if (r == l)
                        {
                            /* a perfect hit: report this one as a match! */
                            db_update_freelist_stats(device, env, entry, f, bm_l * 8, hints);
                            return bm_l * 8;
                        }
                    }
                }
                else
                {
                    /*
                    * simple scan only: tiny range
                    *
                    * Nevertheless, we also have checked our first BYTE,
                    * so we can skip that one
                    */
                    ham_u32_t l = bm_l + 1; /* we have checked the START byte already */
                    ham_u32_t r = l + min_slice_width8 - 1; /* EXCLUSIVE upper bound */

                    /* linear forward validation scan */
                    for ( ; l < r; l++)
                    {
                        hints->cost++;

                        if (p8[l] != 0xFFU)
                            break;
                    }
                    if (r == l)
                    {
                        /* a perfect hit: report this one as a match! */
                        db_update_freelist_stats(device, env, entry, f, bm_l * 8, hints);
                        return bm_l * 8;
                    }
                }

                /*
                * when we get here, we've failed the inner sequence
                * validation of an aligned search; all we can do now is try
                * again at the next aligned scan location.
                *
                * This is the simplest post-backtrack skip of the bunch,
                * Sunday/Hume-wise, but nothing improves upon this (unless
                * we were scanning a size span in there which would've been
                * larger than the skip step here, and that NEVER happens
                * thanks to our prep work at the start of this function.
                */
                bm_l += min_slice_width8;

                /*
                * we know which start positions are viable; we only
                * inspect those.
                *
                * Besides, we assume ALIGNED searches require 1 all-1s
                * byte at least; this improves our skipscan here.
                */
                while (bm_l < bm_r) {
                    hints->cost++;

                    if (p8[bm_l] == 0xFFU)
                    {
                        /*
                        * BM: a hit: see if we have a sufficiently large
                        * free zone here
                        */
                        break;
                    }

                    /* BM: a miss: skip to next opportunity sequentially */
                    bm_l += min_slice_width8;
                }

                if (bm_l >= bm_r) {
                    /* report our failure to find a free slot */
                    db_update_freelist_stats_fail(device, env, entry, f, hints);
                    return (-1);
                }
            }
        }
    } /* hints->aligned */
    else
    {
        /*
         * UNALIGNED search:
         *
         * now there's two flavors in here, or should I say 3?
         *
         * (1) a search for sizes which span ONE all-1s QWORD at least
         * (i.e. searches for size >= sizeof(2 QWORDS)),
         *
         * (2) a search for sizes which are smaller, but still require
         * spanning an entire BYTE (i.e. searches for size >= sizeof(2
         * BYTES)),
         *
         * (3) a search for sizes even tinier than that
         */
        if (size_bits >= 2 * 64)
        {
            /* l & r: INCLUSIVE + EXCLUSIVE boundary; probe END markers */
            ham_u32_t min_slice_width64 = min_slice_width / 64; /* roundDOWN */
            ham_u32_t bm_l = start / 64;
            ham_u32_t bm_r = (end + 64 - 1) / 64;
            ham_u32_t lb = bm_l;
            bm_l += min_slice_width64 - 1; /* first END marker to probe */

            /*
             * we know which END positions are viable; we only
             * inspect those.
             *
             * Besides, we know these UNALIGNED searches require 1
             * all-1s qword at least; this improves our skipscan
             * here.
             */
            while (bm_l < bm_r) {
                hints->cost++;

                if (p64[bm_l] == 0xFFFFFFFFFFFFFFFFULL)
                {
                    /*
                     * BM: a hit: see if we have a sufficiently
                     * large free zone here.
                     */
                    break;
                }

                /* BM: a miss: skip to next opportunity sequentially */
                bm_l += min_slice_width64;
            }

            if (bm_l >= bm_r) {
                /* report our failure to find a free slot */
                db_update_freelist_stats_fail(device, env, entry, f, hints);
                return (-1);
            }

            /* BM search with a startup twist already done */
            for (;;) {
                /* -1 because we have checked the END qword already */
                register ham_u32_t r = bm_l - 1;
                /* +l: INCLUSIVE lower bound */
                register ham_u32_t l = bm_l - min_slice_width64 + 1;

                ham_assert(bm_l > 0, (0));
                ham_assert(bm_l >= min_slice_width64 - 1, (0));
                ham_assert(p64[bm_l] == 0xFFFFFFFFFFFFFFFFULL, (0));

                hints->cost++;

                /*
                 * compare comment in aligned search code
                 *
                 * This time we REV scan down to find the lower bound
                 * of the current range. Also note that our REV guard is
                 * the inverse of the FWD guard: starting close by and
                 * testing at an increasing pace away from the bm_l
                 * probe location.
                 *
                 * Once we've established the lower bound, we FWD scan
                 * past the current probe to see if the entire requested
                 * range is available at this locality.
                 */
                if (min_slice_width64 > SIMPLE_SCAN_THRESHOLD)
                {
                    ham_u32_t d = 1;
                    for (;;)
                    {
                        hints->cost++;
    
                        if (p64[r] != 0xFFFFFFFFFFFFFFFFULL)
                        {
                            l = r + 1; /* lowest PROBABLY okay probe location */
                            break;
                        }
                        if (r < l + d)
                        {
                            if (r < l + 1)
                            {
                                /* l == lowest PROBABLY okay probe location */
                                break;
                            }
                            else
                            {
                                d = 1;
                            }
                        }
                        r -= d;
                        d <<= 1; /* increase step size by a power of 2; inverted divide and conquer */
                    }
                    /*
                     * the guard check adjusted our expected lower
                     * bound in 'l'.
                     *
                     * WARNING: note that due to the way the guard
                     * check loop was coded, we are now SURE the initial
                     * QWORD
                     * _and_ QWORD[bm_l-1] are all-1s at least, so we
                     * don't have to linear-scan those again. However,
                     * we 'lost' the QWORD[bm_l-1] info as the guard
                     * scan went on, so we have to rescan that one again
                     * anyway.
                     *
                     * REV linear validation scan follows...
                     */
                }
                else
                {
                    /*
                     * min_slice_width64 <= SIMPLE_SCAN_THRESHOLD
                     *
                     * we know, however, that we have the need for at
                     * least 1 all-1s qword
                     */
#if 0
                    r = bm_l - 1; /* we have checked the END qword already */
                    l = bm_l - min_slice_width64 + 1; /* INCLUSIVE lower bound */
#endif
                    /*
                     * REV linear validation scan follows...
                     */
                }

                /*
                 * REV linear validation scan:
                 */
                ham_assert(bm_l > 0, (0));
                for (r = bm_l - 1; r > l; r--)
                {
                    hints->cost++;

                    if (p64[r] != 0xFFFFFFFFFFFFFFFFULL)
                    {
                        l = r + 1; /* lowest (last) okay probe location */
                        break;
                    }
                }
                /* fringe case check: the lowest QWORD... */
                if (r == l && p64[r] != 0xFFFFFFFFFFFFFFFFULL)
                {
                    l = r + 1; /* lowest (last) okay probe location */
                }

                /* do we need more 'good space' FWD? */
                if ((++bm_l - l) * 64 < size_bits)
                {
                    /*
                     * FWD linear validation scan:
                     *
                     * try to scan a range which also spans any
                     * possibly extra bits in the non-qword aligned
                     * request size. There's no harm in scanning one
                     * more qword FWD in here, anyway, as we use it to
                     * adjust the next skip on failure anyway.
                     */
                    r = bm_l + min_slice_width64;
                    if (r > bm_r)
                    {
                        r = bm_r;
                    }
                    for ( ; r > bm_l; bm_l++)
                    {
                        hints->cost++;

                        if (p64[bm_l] != 0xFFFFFFFFFFFFFFFFULL)
                        {
                            break;
                        }
                    }
                }

                /*
                 * 'bm_l' now points +1 PAST the position for the LAST
                 * all-1s qword.
                 *
                 * But first: see if we can hug the lead space to a '0'
                 * bit:
                 * 'l' points at the lowest all-1s qword; if it's not
                 * sitting on the lower boundary, then inspect the qword
                 * below that.
                 */
                if (l > lb)
                {
                    /*
                     * get fancy: as we perform an unaligned scan, we
                     * MAY have some more bits sitting in this spot, as
                     * long as they are consecutive with the all-1s
                     * qword up next.
                     *
                     * Right here, it's ENDIANESS that's right dang
                     * important, y'all. And there's a cheaper way to
                     * check if the top bit has been set ya ken: two's
                     * complement sign check, right on!
                     */
                    ham_u32_t lpos = BITSCAN_MSBit(ham_db2h64(p64[l-1]), l * 64);
                    ham_assert(l > 0, (0));

                    /* do we have enough free space now? */
                    ham_assert(bm_l > 0, (0));
                    ham_assert((bm_l - 1) * 64 >= lpos, (0));
                    if (size_bits <= (bm_l - 1) * 64 - lpos)
                    {
                        /* yeah! */
                        db_update_freelist_stats(device, env, entry, f, lpos, hints);
                        return lpos;
                    }

                    /*
                     * second, we still ain't got enough space, so we
                     * MUST count the tail bits at [bm_l] -- at least if
                     * we haven't hit the upper bound yet.
                     *
                     * But only do the (expensive) bitscan when we just
                     * need those few extra bits in there to accomplish
                     * our goal.
                     */
                    if (bm_l >= bm_r)
                    {
                        /* upper bound hit: we won't be able to report a match. */
                        db_update_freelist_stats_fail(device, env, entry, f, hints);
                        return (-1);
                    }
                    else /* if (size_bits <= bm_l * 64 - lpos) */
                    {
                        ham_u32_t rpos = BITSCAN_LSBit(ham_db2h64(p64[bm_l]), bm_l * 64);
                        ham_assert(bm_l > 0, (0));
                        ham_assert(rpos >= lpos, (0));
                        /*
                         * Special assumption! When the 'end' is NOT on
                         * a qword boundary, we assume the entire qword
                         * is still filled correctly, which means: any
                         * bits in there BEYOND 'end'
                         * are still correct 0s and 1s. At least we
                         * assume they are all _accessible_; as we are
                         * conservative, we _do_ limit rpos to 'end' as
                         * the stats hinter gave it to us.
                         */
                        if (rpos > end)
                            rpos = end;
                        ham_assert(rpos >= lpos, (0));

                        /* again: do we have enough free space now? */
                        if (size_bits <= rpos - lpos)
                        {
                            /* yeah! */
                            db_update_freelist_stats(device, env, entry, f, lpos, hints);
                            return lpos;
                        }
                    }
                }
                else
                {
                    /* do we have enough free space now? */
                    if (size_bits <= (bm_l - l) * 64)
                    {
                        /* yeah! */
                        db_update_freelist_stats(device, env, entry, f, l * 64, hints);
                        return l * 64;
                    }

                    /*
                     * second, we still ain't got enough space, so we
                     * MUST count the tail bits at [bm_l] -- at least if
                     * we haven't hit the upper bound yet.
                     *
                     * But only do the (expensive) bitscan when we just
                     * need those few extra bits in there to accomplish
                     * our goal.
                     */
                    if (bm_l >= bm_r)
                    {
                        /* upper bound hit: we won't be able to report a match. */
                        db_update_freelist_stats_fail(device, env, entry, f, hints);
                        return (-1);
                    }
                    else /* if (size_bits <= (bm_l - l) * 64) */
                    {
                        ham_u32_t rpos = BITSCAN_LSBit(ham_db2h64(p64[bm_l]),
                                bm_l * 64);
                        ham_assert(bm_l > 0, (0));
                        ham_assert(rpos >= l * 64, (0));
                        /*
                         * Special assumption! When the 'end' is NOT on
                         * a qword boundary, we assume the entire qword
                         * is still filled correctly, which means: any
                         * bits in there BEYOND 'end'
                         * are still correct 0s and 1s. At least we
                         * assume they are all _accessible_; as we are
                         * conservative, we _do_ limit rpos to 'end' as
                         * the stats hinter gave it to us.
                         */
                        if (rpos > end)
                            rpos = end;
                        ham_assert(rpos >= l * 64, (0));

                        /* again: do we have enough free space now? */
                        ham_assert(rpos >= l * 64, (0));
                        if (size_bits <= rpos - l * 64)
                        {
                            /* yeah! */
                            db_update_freelist_stats(device, env, entry, f, l * 64, hints);
                            return l * 64;
                        }
                    }
                }

                /*
                 * otherwise, we can determine the new skip value: our
                 * next probe should be here:
                 */
                bm_l += min_slice_width64;

                /* BM skipscan */
                while (bm_l < bm_r) {
                    hints->cost++;

                    if (p64[bm_l] == 0xFFFFFFFFFFFFFFFFULL)
                    {
                        /*
                         * BM: a hit: see if we have a sufficiently
                         * large free zone here.
                         */
                        break;
                    }

                    /* BM: a miss: skip to next opportunity sequentially */
                    bm_l += min_slice_width64;
                }

                if (bm_l >= bm_r) {
                    /* report our failure to find a free slot */
                    db_update_freelist_stats_fail(device, env, entry, f, hints);
                    return (-1);
                }
            }
        }
        else if (size_bits >= 2 * 8)
        {
            ham_u8_t *p8=(ham_u8_t *)p64;

            /* l & r: INCLUSIVE + EXCLUSIVE boundary; probe END markers */
            ham_u32_t min_slice_width8 = min_slice_width / 8; /* roundDOWN */
            ham_u32_t bm_l = start / 8;
            ham_u32_t bm_r = (end + 8 - 1) / 8;
            ham_u32_t lb = bm_l;
            ham_assert(min_slice_width8 > 0, (0));
            bm_l += min_slice_width8 - 1; /* first END marker to probe */

            /*
             * we know which END positions are viable; we only
             * inspect those.
             *
             * Besides, we know these UNALIGNED searches require 1
             * all-1s BYTE at least; this improves our skipscan
             * here.
             */
            while (bm_l < bm_r) {
                hints->cost++;

                if (p8[bm_l] == 0xFFU)
                {
                    /*
                     * BM: a hit: see if we have a sufficiently
                     * large free zone here.
                     */
                    break;
                }

                /* BM: a miss: skip to next opportunity sequentially */
                bm_l += min_slice_width8;
            }

            if (bm_l >= bm_r) {
                /* report our failure to find a free slot */
                db_update_freelist_stats_fail(device, env, entry, f, hints);
                return (-1);
            }

            /* BM search with a startup twist already done */
            for (;;) {
                /* -1 because we have checked the END byte already */
                register ham_u32_t r = bm_l - 1;
                /* +1 because INCLUSIVE lower bound */
                register ham_u32_t l = bm_l - min_slice_width8 + 1;

                ham_assert(bm_l > 0, (0));
                ham_assert(bm_l >= min_slice_width8 - 1, (0));
                ham_assert(p8[bm_l] == 0xFFU, (0));

                /*
                 * compare comment in aligned search code
                 *
                 * This time we REV scan down to find the lower bound
                 * of the current range. Also note that our REV guard is
                 * the inverse of the FWD guard: starting close by and
                 * testing at an increasing pace away from the bm_l
                 * probe location.
                 *
                 * Once we've established the lower bound, we FWD scan
                 * past the current probe to see if the entire requested
                 * range is available at this locality.
                 */
                if (min_slice_width8 > SIMPLE_SCAN_THRESHOLD)
                {
                    ham_u32_t d = 1;
                    for (;;)
                    {
                        hints->cost++;

                        if (p8[r] != 0xFFU)
                        {
                            l = r + 1; /* lowest PROBABLY okay probe location */
                            break;
                        }
                        if (r < l + d)
                        {
                            if (r < l + 1)
                            {
                                /* l == lowest PROBABLY okay probe location */
                                break;
                            }
                            else
                            {
                                d = 1;
                            }
                        }
                        r -= d;
                        d <<= 1; /* increase step size by a power of 2; inverted divide and conquer */
                    }
                    /*
                     * the guard check adjusted our expected lower
                     * bound in 'l'.
                     *
                     * WARNING: note that due to the way the guard
                     * check loop was coded, we are now SURE the initial
                     * BYTE
                     * _and_ BYTE[bm_l-1] are all-1s at least, so we
                     * don't have to linear-scan those again. However,
                     * we 'lost' the BYTE[bm_l-1] info as the guard
                     * scan went on, so we have to rescan that one again
                     * anyway.
                     *
                     * REV linear validation scan follows...
                     */
                }
                else
                {
                    /*
                     * min_slice_width8 <= SIMPLE_SCAN_THRESHOLD
                     *
                     * we know, however, that we have the need for at
                     * least 1 all-1s byte
                     */
#if 0
                    r = bm_l - 1; /* we have checked the END byte already */
                    l = bm_l - min_slice_width8 + 1; /* INCLUSIVE lower bound */
#endif
                    /*
                     * REV linear validation scan follows...
                     */
                }

                /*
                 * REV linear validation scan:
                 */
                ham_assert(bm_l > 0, (0));
                for (r = bm_l - 1; r > l; r--)
                {
                    hints->cost++;

                    if (p8[r] != 0xFFU)
                    {
                        l = r + 1; /* lowest (last) okay probe location */
                        break;
                    }
                }
                /* fringe case check: the lowest BYTE... */
                if (r == l && p8[r] != 0xFFU)
                {
                    l = r + 1; /* lowest (last) okay probe location */
                }

                /* do we need more 'good space' FWD? */
                if ((++bm_l - l) * 8 < size_bits)
                {
                    /*
                     * FWD linear validation scan:
                     *
                     * try to scan a range which also spans any
                     * possibly extra bits in the non-byte aligned
                     * request size. There's no harm in scanning one
                     * more byte FWD in here, anyway, as we use it to
                     * adjust the next skip on failure anyway.
                     */
                    r = bm_l + min_slice_width8;
                    if (r > bm_r)
                    {
                        r = bm_r;
                    }
                    for ( ; r > bm_l; bm_l++)
                    {
                        hints->cost++;

                        if (p8[bm_l] != 0xFFU)
                        {
                            break;
                        }
                    }
                }

                /*
                 * 'bm_l' now points +1 PAST the position for the LAST
                 * all-1s byte.
                 *
                 * But first: see if we can hug the lead space to a '0'
                 * bit:
                 * 'l' points at the lowest all-1s byte; if it's not
                 * sitting on the lower boundary, then inspect the byte
                 * below that.
                 */
                if (l > lb)
                {
                    /*
                     * get fancy: as we perform an unaligned scan, we
                     * MAY have some more bits sitting in this spot, as
                     * long as they are consecutive with the all-1s byte
                     * up next.
                     *
                     * Right here, ENDIANESS doesn't matter at all. And
                     * there's a cheaper way to check if the top bit has
                     * been set ya ken: two's complement sign check,
                     * right on!
                     */
                    ham_u32_t lpos = BITSCAN_MSBit8(p8[l-1], l * 8);
                    ham_assert(l > 0, (0));
                    ham_assert(bm_l > 0, (0));
                    ham_assert((bm_l - 1) * 8 >= lpos, (0));

                    /* do we have enough free space now? */
                    if (size_bits <= (bm_l - 1) * 8 - lpos)
                    {
                        /* yeah! */
                        db_update_freelist_stats(device, env, entry, f, lpos, hints);
                        return lpos;
                    }

                    /*
                     * second, we still ain't got enough space, so we
                     * MUST count the tail bits at [bm_l] -- at least if
                     * we haven't hit the upper bound yet.
                     *
                     * But only do the (expensive) bitscan when we just
                     * need those few extra bits in there to accomplish
                     * our goal.
                     */
                    if (bm_l >= bm_r)
                    {
                        /* upper bound hit: we won't be able to report a match. */
                        db_update_freelist_stats_fail(device, env, entry, f, hints);
                        return (-1);
                    }
                    else /* if (size_bits <= bm_l * 8 - lpos) */
                    {
                        ham_u32_t rpos = BITSCAN_LSBit8(p8[bm_l], bm_l * 8);
                        ham_assert(bm_l > 0, (0));
                        ham_assert(rpos >= lpos, (0));
                        /*
                         * Special assumption! When the 'end' is NOT on
                         * a qword boundary, we assume the entire qword
                         * is still filled correctly, which means: any
                         * bits in there BEYOND 'end'
                         * are still correct 0s and 1s. At least we
                         * assume they are all _accessible_; as we are
                         * conservative, we _do_ limit rpos to 'end' as
                         * the stats hinter gave it to us.
                         */
                        if (rpos > end)
                            rpos = end;
                        ham_assert(rpos >= lpos, (0));

                        /* again: do we have enough free space now? */
                        if (size_bits <= rpos - lpos)
                        {
                            /* yeah! */
                            db_update_freelist_stats(device, env, entry, f, lpos, hints);
                            return lpos;
                        }
                    }
                }
                else
                {
                    /* do we have enough free space now? */
                    if (size_bits <= (bm_l - l) * 8)
                    {
                        /* yeah! */
                        db_update_freelist_stats(device, env, entry, f, l * 8, hints);
                        return l * 8;
                    }

                    /*
                     * second, we still ain't got enough space, so we
                     * MUST count the tail bits at [bm_l] -- at least if
                     * we haven't hit the upper bound yet.
                     *
                     * But only do the (expensive) bitscan when we just
                     * need those few extra bits in there to accomplish
                     * our goal.
                     */
                    if (bm_l >= bm_r)
                    {
                        /* upper bound hit: we won't be able to report a match. */
                        db_update_freelist_stats_fail(device, env, entry, f, hints);
                        return (-1);
                    }
                    else /* if (size_bits <= (bm_l - l) * 8) */
                    {
                        ham_u32_t rpos = BITSCAN_LSBit8(p8[bm_l], bm_l * 8);
                        ham_assert(bm_l > 0, (0));
                        ham_assert(rpos >= l * 8, (0));
                        /*
                         * Special assumption! When the 'end' is NOT on
                         * a qword boundary, we assume the entire qword
                         * is still filled correctly, which means: any
                         * bits in there BEYOND 'end'
                         * are still correct 0s and 1s. At least we
                         * assume they are all _accessible_; as we are
                         * conservative, we _do_ limit rpos to 'end' as
                         * the stats hinter gave it to us.
                         */
                        if (rpos > end)
                            rpos = end;
                        ham_assert(rpos >= l * 8, (0));

                        /* again: do we have enough free space now? */
                        if (size_bits <= rpos - l * 8)
                        {
                            /* yeah! */
                            db_update_freelist_stats(device, env, entry, f, l * 8, hints);
                            return l * 8;
                        }
                    }
                }

                /*
                 * otherwise, we can determine the new skip value: our
                 * next probe should be here:
                 */
                bm_l += min_slice_width8;

                /* BM skipscan */
                while (bm_l < bm_r) {
                    hints->cost++;

                    if (p8[bm_l] == 0xFFU)
                    {
                        /*
                         * BM: a hit: see if we have a sufficiently
                         * large free zone here.
                         */
                        break;
                    }

                    /* BM: a miss: skip to next opportunity sequentially */
                    bm_l += min_slice_width8;
                }

                if (bm_l >= bm_r) {
                    /* report our failure to find a free slot */
                    db_update_freelist_stats_fail(device, env, entry, f, hints);
                    return (-1);
                }
            }
        }
        else if (size_bits > 1)
        {
            ham_u8_t *p=(ham_u8_t *)p64;

            /* l & r: INCLUSIVE + EXCLUSIVE boundary; probe END markers */
            ham_u32_t bm_l = start;
            ham_u32_t bm_r = end;
            ham_assert(min_slice_width > 0, (0));
            bm_l += min_slice_width - 1; /* first END marker to probe */

            /*
             * we know which END positions are viable; we only
             * inspect those.
             */
            for (;;) {
                hints->cost++;

                /*
                 * the 'byte level front scanner':
                 */
                if (!p[bm_l >> 3])
                {
                    /*
                     * all 0 bits in there. adjust skip
                     * accordingly. But first we scan further at
                     * byte level, as we assume 0-bytes come in
                     * clusters:
                     */
                    ham_u32_t ub = (bm_r >> 3); /* EXCLUSIVE bound */
                    bm_l >>= 3;
                    if (min_slice_width <= 8)
                    {
                        for (bm_l++; bm_l < ub && !p[bm_l]; bm_l++)
                        {
                            hints->cost++;
                        }
                    }
                    else
                    {
                        /*
                         * at a spacing of 9 bits or more, we can
                         * skip bytes in the scanner and still be
                         * down with it.
                         */
                        ham_assert(min_slice_width < 16, (0));
                        for (bm_l += 2; bm_l < ub && !p[bm_l]; bm_l += 2)
                        {
                            hints->cost++;
                        }
                    }
                    
                    /*
                     * BM: a miss: skip to next opportunity
                     * sequentially:
                     * first roundUP bm_l to the start of the next
                     * byte:
                     */
                    bm_l <<= 3;

                    /*
                     * as bm_l now points to the bit just PAST the
                     * currently known '0'-series (the byte), it MAY
                     * be a '1', so compensate for that by reducing
                     * the next part of the skip:
                     */
                    bm_l += min_slice_width - 1;

                    if (bm_l >= bm_r)
                    {
                        /* report our failure to find a free slot */
                        db_update_freelist_stats_fail(device, env, entry, f, hints);
                        return (-1);
                    }
                    continue;
                }

                /* the regular BM scanloop */
                if (p[bm_l >> 3] & (1 << (bm_l & 0x07)))
                {
                    /*
                     * BM: a hit: see if we have a sufficiently
                     * large free zone here.
                     */
                    break;
                }
                else
                {
                    /* BM: a miss: skip to next opportunity sequentially */
                    bm_l += min_slice_width;
                    if (bm_l >= bm_r)
                    {
                        /* report our failure to find a free slot */
                        db_update_freelist_stats_fail(device, env, entry, f, hints);
                        return (-1);
                    }
                }
            }

            /* BM search with a startup twist already done */
            for (;;) {
                /* -1 because we have checked the END BIT already */
                register ham_u32_t r = bm_l - 1;
                /* +1 because INCLUSIVE lower bound */
                register ham_u32_t l = bm_l - min_slice_width + 1;

                ham_assert(bm_l > 0, (0));
                ham_assert(bm_l >= min_slice_width - 1, (0));
                ham_assert(p[bm_l >> 3] & (1 << (bm_l & 0x07)), (0));

                hints->cost++;

                /*
                 * compare comment in aligned search code
                 *
                 * This time we REV scan down to find the lower bound
                 * of the current range. Also note that our REV guard is
                 * the inverse of the FWD guard: starting close by and
                 * testing at an increasing pace away from the bm_l
                 * probe location.
                 *
                 * Once we've established the lower bound, we FWD scan
                 * past the current probe to see if the entire requested
                 * range is available at this locality.
                 */
                if (min_slice_width > SIMPLE_SCAN_THRESHOLD)
                {
                    ham_u32_t d = 1;
                    for (;;)
                    {
                        hints->cost++;

                        if (!(p[r >> 3] & (1 << (r & 0x07))))
                        {
                            l = r + 1; /* lowest PROBABLY okay probe location */
                            break;
                        }
                        if (r < l + d)
                        {
                            if (r < l + 1)
                            {
                                /* l == lowest PROBABLY okay probe location */
                                break;
                            }
                            else
                            {
                                d = 1;
                            }
                        }
                        r -= d;
                        d <<= 1; /* increase step size by a power of 2; inverted divide and conquer */
                    }
                    /*
                     * the guard check adjusted our expected lower
                     * bound in 'l'.
                     *
                     * WARNING: note that due to the way the guard
                     * check loop was coded, we are now SURE the initial
                     * BIT
                     * _and_ BIT[bm_l-1] are all-1s at least, so we
                     * don't have to linear-scan those again. However,
                     * we 'lost' the BIT[bm_l-1] info as the guard
                     * scan went on, so we have to rescan that one again
                     * anyway.
                     *
                     * REV linear validation scan follows...
                     */
                }
                else
                {
                    /*
                     * min_slice_width8 <= SIMPLE_SCAN_THRESHOLD
                     *
                     * we know, however, that we have the need for at
                     * least 1 all-1s BIT
                     */
#if 0
                    r = bm_l - 1; /* we have checked the END bit already */
                    l = bm_l - min_slice_width + 1; /* INCLUSIVE lower bound */
#endif
                    /*
                     * REV linear validation scan follows...
                     */
                }

                /*
                 * REV linear validation scan:
                 */
                ham_assert(bm_l > 0, (0));
                for (r = bm_l - 1; r > l; r--)
                {
                    hints->cost++;

                    if (!(p[r >> 3] & (1 << (r & 0x07))))
                    {
                        l = r + 1; /* lowest (last) okay probe location */
                        break;
                    }
                }
                /* fringe case check: the lowest BIT... */
                if (r == l && !(p[r >> 3] & (1 << (r & 0x07))))
                {
                    l = r + 1; /* lowest (last) okay probe location */
                }

                /* do we need more 'good space' FWD? */
                if ((++bm_l - l) < size_bits)
                {
                    /*
                     * FWD linear validation scan:
                     */
                    r = bm_l + min_slice_width - 1;
                    if (r > bm_r)
                    {
                        r = bm_r;
                    }
                    for ( ; r > bm_l; bm_l++)
                    {
                        hints->cost++;

                        if (!(p[bm_l >> 3] & (1 << (bm_l & 0x07))))
                        {
                            break;
                        }
                    }
                }

                /*
                 * 'bm_l' now points +1 PAST the position for the LAST
                 * '1' bit.
                 *
                 * But first: As we are scanning at bit level we are
                 * already hugging the lead space to a '0' bit:
                 * 'l' points at the lowest '1' bit.
                 */

                /* do we have enough free space now? */
                if (size_bits <= (bm_l - l))
                {
                    /* yeah! */
                    db_update_freelist_stats(device, env, entry, f, l, hints);
                    return l;
                }

                /*
                 * otherwise, we can determine the new skip value: our
                 * next probe should be here:
                 */
                bm_l += min_slice_width;

                /* BM skipscan */
                while (bm_l < bm_r)
                {
                    hints->cost++;

                    /*
                    the 'byte level front scanner':
                    */
                    if (!p[bm_l >> 3])
                    {
                        /*
                         * all 0 bits in there. adjust skip
                         * accordingly. But first we scan further at
                         * byte level, as we assume 0-bytes come in
                         * clusters:
                         */
                        ham_u32_t ub = (bm_r >> 3); /* EXCLUSIVE bound */
                        bm_l >>= 3;
                        for (bm_l++; bm_l < ub && !p[bm_l]; bm_l++)
                        {
                            hints->cost++;
                        }

                        /*
                         * BM: a miss: skip to next opportunity
                         * sequentially:
                         * first roundUP bm_l to the start of the next
                         * byte:
                         */
                        bm_l <<= 3;

                        /*
                         * as bm_l now points to the bit just PAST the
                         * currently known '0'-series (the byte), it MAY
                         * be a '1', so compensate for that by reducing
                         * the next part of the skip:
                         */
                        bm_l += min_slice_width - 1;
                        continue;
                    }

                    if (p[bm_l >> 3] & (1 << (bm_l & 0x07)))
                    {
                        /*
                         * BM: a hit: see if we have a sufficiently
                         * large free zone here.
                         */
                        break;
                    }
                    else
                    {
                        /* BM: a miss: skip to next opportunity sequentially */
                        bm_l += min_slice_width;
                    }
                }

                /*
                 * we still ain't got enough space, but we
                 * already counted all the tail bits at [bm_l] -- if we
                 * haven't hit the upper bound already.
                 */
                if (bm_l >= bm_r)
                {
                    /* upper bound hit: we won't be able to report a match. */
                    db_update_freelist_stats_fail(device, env, entry, f, hints);
                    return (-1);
                }
            }
        }
        else /* if (size_bits == 1) */
        {
            ham_u8_t *p=(ham_u8_t *)p64;

            /* l & r: INCLUSIVE + EXCLUSIVE boundary; probe END markers */
            ham_u32_t bm_l = start;
            ham_u32_t bm_r = end;
            ham_assert(min_slice_width > 0, (0));

            /*
             * We can do some special things for single-bit slot
             * search;
             * besides that, it would trigger all sorts of subtle
             * nastiness in the section above which handles requests for
             * 2 bits or more, one of the major ones being END==START
             * marker, causing unsigned integer wrap-arounds due to the
             * REVerse scan, etc. done up there.
             *
             * Never mind that; a single-bit search is a GOOD thing to
             * specialize on: tiny keys (any keys which fits in the
             * default 21 bytes hamsterdb
             * reserves for keys) do not need the (slow) REV/FWD
             * bitscans we have to do otherwise. The fun here is that
             * looking for a single
             * '1' bit is the same as looking for ANYTHING that is NOT
             * ZERO.
             *
             * Which means we can go for the jugular here and take
             * either the QWORD scan or 'native integer' size as a
             * scanner basic inspection chunk: when we have thus
             * ascertained a hit, all we need to do is determine _which_
             * bit caused the non-zero-ness of such a multi-byte integer
             * value.
             *
             * Having said that, there's another interesting bit here:
             * since START==END, the prescan is pretty useless... or
             * put in equivalent terms: the prescan IS the ENTIRE scan:
             * since we will hit that sought-after '1'-bit in the
             * prescan for certain, the entire main scan loop can be
             * discarded.
             *
             * And last but not least: we can still apply the prescan
             * optimizations as we do them otherwise; any scheme which
             * is not skipping bytes (and thereby introducing
             * sparseness) is identical to a straight-forward linear
             * scan, due to the pattern width == 1. That means we don't
             * need to perform any fancy footwork here, unless we think
             * we have something that's orders of magnitude better than
             * a linear scan and still promises some reasonable
             * results -- all I can think of here is the binary search
             * 'fast prescan' alternative here, as BM (Boyer-Moore)
             * just lost it, all the way.
             *
             * Anyway, the biggest speed gain we can get is due to the
             * statistics gatherer, which can hint us where to start
             * looking the next time around.
             *
             * The statistics gatherer/hinter does not help with
             * pathological cases such as (create a large filled space, then
             * apply pattern
             * 'write 2 keys, delete 1 key' repetivily, so that each
             * two inserts lands one in the 1-bit gap produced at the
             * start of the file due to the delete/erase operation,
             * while the other insert will have to happen at the end --
             * the only way to cope with this kind of pathology is set
             * 'FAST' mode, which blatantly ignores free space created
             * by 'delete/erase' and have the statistics gatherer know
             * then which free slots are generated 'sufficiently large'
             * to be noted and taken into account for adjusting the
             * where-to-start-looking-next index offset.
             */

            /* bm_l == first END marker to probe (size == 1) */

            /*
             * we know we'll have check each bit, pardon, byte in
             * there. BM is no help, au contraire mon ami, so we sit
             * down and build ourselves a fast byte-level sequential
             * scanner. A bit of Duff's Device inspiration is all
             * that's left to us for speeding this mother up in a
             * portable fashion, i.e. without reverting to ASM
             * coding.
             *
             * given that we HOPE our statistics gatherer/hinter is
             * smart enough to position us NEAR a good spot, it's no
             * use to unroll the scanner into a multi-stage beast
             * where we scan the edges at byte-level, while scanning
             * the core bulk in qword-aligned fashion:
             * we can't simply do qwords all the time as there are
             * CPUs out there that throw a tantrum when addressing
             * integers at odd-address boundaries (several of the
             * CPUs in the MC68K series, for example).
             */
            if (min_slice_width <= 16)
            {
                /* the usual; just step on it using Duff's Device (loop unrolling) */
                ham_u32_t l = (bm_l >> 3);
                ham_u32_t r = ((bm_r + 8 - 1) >> 3);
                ham_u8_t *e = p + r;
                p += l;

                ham_assert(r > l, (0));
                /* cost is low as this is a cheap loop anyway */
                hints->cost += (r - l + 8 - 1) / 8;

                switch ((r - l)  & 0x07)
                {
                case 0:
                    while (p != e)
                    {
                    if (*p++) break;
                case 7:
                    if (*p++) break;
                case 6:
                    if (*p++) break;
                case 5:
                    if (*p++) break;
                case 4:
                    if (*p++) break;
                case 3:
                    if (*p++) break;
                case 2:
                    if (*p++) break;
                case 1:
                    if (*p++) break;
                    }
                    break;
                }
                p--; /* correct p */
                ham_assert(p < e, (0));
                if (!p[0])
                {
                    /* we struck end of loop without a hit!
                    report our failure to find a free slot */
                    db_update_freelist_stats_fail(device, env, entry, f, hints);
                    return (-1);
                }

                /*
                 * now we have the byte with the free bit slot;
                 * see which bit it is:
                 */
                l = 8 * (ham_u32_t)(p - ((ham_u8_t *)p64)); /* ADD: the number of all-0 bytes we traversed + START offset */

                ham_assert(p[0], (0)); /* we are sure we will hit a match in here! */

                for (r = 0;; r++)
                {
                    ham_assert(r < 8, (0)); /* we are sure we will hit a match in here! */
                    if (p[0] & (1 << r))
                    {
                        l += r; /* lowest (last) okay probe location */
                        break;
                    }
                }

                ham_assert(bm_l <= l, (0));
                ham_assert(size_bits == 1, (0));
                /* found a slot! */
                db_update_freelist_stats(device, env, entry, f, l, hints);
                return l;
            }
            else
            {
                /*
                 * big skipsize; the same thing once more, but now
                 * without Duff, but that speed gain is compensated
                 * for as we will skip multiple bytes during each
                 * run, which is another, though less accurate, way
                 * to save time in here...
                 */
                ham_u32_t l = (bm_l >> 3);
                ham_u32_t r = ((bm_r + 8 - 1) >> 3);
                ham_u8_t *e = p + r;
                ham_u32_t step = (min_slice_width >> 3);

                p += l;

                ham_assert(r > l, (0));
                /* cost is low as this is a cheap loop anyway */
                hints->cost += (r - l + 8 - 1) / step;

                for (; !*p && p < e; p += step)
                    ;
                if (p >= e)
                {
                    /*
                     * we struck end of loop without a hit!
                     *
                     * report our failure to find a free slot
                     */
                    db_update_freelist_stats_fail(device, env, entry, f, hints);
                    return (-1);
                }

                /*
                 * now we have the byte with the free bit slot;
                 * see which bit it is:
                 */
                l = 8 * (ham_u32_t)(p - ((ham_u8_t *)p64)); /* ADD: the number of all-0 bytes we traversed + START offset */

                ham_assert(p[0], (0)); /* we are sure we will hit a match in here! */

                for (r = 0;; r++)
                {
                    ham_assert(r < 8, (0)); /* we are sure we will hit a match in here! */
                    if (p[0] & (1 << r))
                    {
                        l += r; /* lowest (last) okay probe location */
                        break;
                    }
                }

                ham_assert(bm_l <= l, (0));
                ham_assert(size_bits == 1, (0));
                /* found a slot! */
                db_update_freelist_stats(device, env, entry, f, l, hints);
                return l;
            }
            // should never get here
        }
    }
}

static ham_status_t
__freel_alloc_page32(Page **page_ref, Device *device, Environment *env,
                freelist_cache_t *cache, freelist_entry_t *entry)
{
    ham_size_t i;
    freelist_entry_t *entries=freel_cache_get_entries(cache);
    Page *page=0;
    freelist_payload_t *fp;
    ham_size_t size_bits = __freel_get_freelist_entry_maxspan(device, env, cache);

    ham_assert(((size_bits/8) % sizeof(ham_u64_t)) == 0,
            ("freelist bitarray size must be == 0 MOD sizeof(ham_u64_t) "
             "due to the scan algorithm"));

    *page_ref = 0;

    /*
     * it's not enough just to allocate the page - we have to make sure
     * that the freelist pages build a linked list; therefore we
     * might have to allocate more than just one page...
     *
     * we can skip the first element - it's the root page and always exists
     */
    for (i=1; ; i++)
    {
        ham_assert(i < freel_cache_get_count(cache), (0));

        if (!freel_entry_get_page_id(&entries[i]))
        {
            ham_status_t st;
            Page *prev_page = 0;

            /*
             * load the previous page and the payload object;
             * make the page dirty.

             There's an interesting thing here about db_fetch_page() and
             db_alloc_page() in relation to this code being invoked by
             env_reserve_space():

             when such is done BEFORE, i.e. WITHOUT having a live page
             cache, then this routine will fail in a very NONOBVIOUS
             manner on Win32 platforms at least:

             no page cache means that db_fetch_page() and db_alloc_page()
             are not 'aware' of each other and here it means they will
             then independently alloc a page struct for the same page:
             db_alloc_page() will create the first one on the first round
             through this loop, attach the persisted mmap view of one(1)
             disc page to it and set a few key persisted values.

             Then, on the next round through this loop (and env_reserve_space()
             will most probably result in many rounds here, if any at all),
             it's the 'prev_page' db_fetch_page() alloc-ing ANOTHER page
             struct for the same page: previous round's 'page' is identical
             to this rounds 'prev_page'.

             The dual alloc of a page struct isn't what we worry about, it's
             the fact that Win32 at least does something truely wicked
             alongside: being asked twice to provide a mmap area view to a
             certain disc page N, both invocations will have the OS produce
             two DIFFERENT view addresses. Which, to no surprise, will contain
             different content: the db_fetch_page() produced mmap view to
             the page does NOT contain the edits performed on said page in
             the previous round.

             That is issue #1 when we don't have a live page cache to take care
             of it all; the second issue is that those page views are never
             flushed to disc, because there's nobody around who actually
             FLUSHES those 'dirty' pages, when there's no page cache around.

             Lastly, but completely independently, there's the issue of a TINY
             cache and/or arbitrary cache ageing algorithms: assuming the
             cache will flush 'prev_page' from the cache when 'page' is
             created (which can happen under certain ageing schemes where
             previously fetched pages receive 'future' timestamps so as to make
             them appear younger then they really are, most often done to
             prolong the life expectancy of certain pages in a cache), then
             'fp' will point to Nirvana by the time the overflow address
             gets updated further below.

             The solution for that little conundrum is to temporarily 'lock'
             prev_page into the cache, which is done simply by bumping up
             it's reference count, bumping down again once the update has been
             done and there is no further need for anything related to
             'prev_page'.

             The former issue has a simple answer too: demand a live page
             cache. Which is done, at least in debug builds, by the assert
             at the top of this function's body.
             */
            if (i==1) {
                fp=env->get_freelist();
                __env_set_dirty(env);
            }
            else {
                st=env_fetch_page(&prev_page, env,
                        freel_entry_get_page_id(&entries[i-1]), 0);
                if (st)
                    return (st);
                __page_set_dirty(prev_page);
                fp=page_get_freelist(prev_page);
            }

            /*
             * allocate a new page, fixed the linked list
             */
            st=env_alloc_page(&page, env, Page::TYPE_FREELIST,
                    PAGE_IGNORE_FREELIST|PAGE_CLEAR_WITH_ZERO);
            if (!page)
            {
                ham_assert(st != 0, (0));
                return st;
            }
            ham_assert(st == 0, (0));

            freel_set_overflow(fp, page->get_self());
            /* done editing /previous/ freelist page */

            fp=page_get_freelist(page);
            freel_set_start_address(fp,
                    freel_entry_get_start_address(&entries[i]));
            freel_set_max_bits32(fp, (ham_u32_t)(size_bits));
            __page_set_dirty(page);
            ham_assert(freel_entry_get_max_bits(&entries[i])==
                    freel_get_max_bits32(fp), (0));
            freel_entry_set_page_id(&entries[i], page->get_self());

            ham_assert(cache->_init_perf_data, (0));
            st = cache->_init_perf_data(cache, device, env, &entries[i], fp);
            if (st) {
                return st;
            }
        }

        if (&entries[i]==entry)
        {
            *page_ref = page;
            return HAM_SUCCESS;
        }
    }
}


/**
 * Report if the requested size can be obtained from the given freelist
 * page.
 *
 * Always make use of the collected statistics, but act upon it in
 * different ways, depending on our current 'mgt_mode' setting.
 *
 * Note: the answer is an ESTIMATE, _not_ a guarantee.
 *
 * Return the first cache entry index from now (start_index) where you
 * have a chance of finding a free slot.
 *
 * Note: the initial round with have start_index == -1 incoming.
 *
 * Return -1 to signal there's no chance at all.
 */
static ham_s32_t
__freel_locate_sufficient_free_space(freelist_hints_t *dst,
        freelist_global_hints_t *hints,
        Device *device, Environment *env, freelist_cache_t *cache,
        ham_s32_t start_index)
{
    freelist_entry_t *entry;

    if (hints->max_rounds == 0
        || freel_cache_get_count(cache) <
                hints->start_entry + hints->page_span_width) {
        /* it's the end of the road for this one */
        return -1;
    }

    ham_assert(hints->max_rounds <= freel_cache_get_count(cache), (0));

    for (;; hints->max_rounds--) {
        if (hints->max_rounds == 0) {
            /* it's the end of the road for this one */
            return -1;
        }

        if (dam_is_set(hints->mgt_mode, HAM_DAM_SEQUENTIAL_INSERT)) {
            if (1) {
                /*
                 * SEQUENTIAL:
                 *
                 * assume the last pages have the optimum chance to
                 * serve a suitable free chunk: start at the last
                 * freelist page and scan IN REVERSE to locate a
                 * suitable freelist page of the bunch at the tail end
                 * (~ latest entries) of the freelist page collective.
                 *
                 * Usually, this will get you a VERY strong preferrence
                 * for the last freelist page, but when that one gets
                 * filled up, we postpone the need to allocate extra
                 * storage on disc by checking out the 'older' freelist
                 * pages as well: those may have a few free slots
                 * available, assuming there've been
                 * records deleted (erased) before now.
                 *
                 * In a sense, this mode is good for everyone: it
                 * quickly finds free space, while still utilizing all
                 * the free space available in the current DB file,
                 * before we go off and require the DB file to be expanded.
                 */
                if (start_index == -1)
                {
                    /* first round: position ourselves at the end of the list: */
                    start_index = freel_cache_get_count(cache) - hints->page_span_width;
                }
                else
                {
                    start_index -= hints->skip_init_offset;
                    /* only apply the init_offset at the first increment
                     * cycle to break repetitiveness */
                    hints->skip_init_offset = 0;

                    start_index -= hints->skip_step;
                    /*
                     * We don't have to be a very good SRNG here, so
                     * the 32-bit int wrap around and the case where the
                     * result lands below the 'start_index' limit are resolved in an (overly)
                     * simple way:
                     */
                    if (start_index < 0) {
                        /* we happen to have this large prime which we'll
                         * assume will be larger than any sane freelist entry
                         * list will ever get in this millenium ;-) */
                        start_index += 295075153;
                    }
                    start_index %= (freel_cache_get_count(cache) - hints->start_entry - hints->page_span_width + 1);
                    start_index += hints->start_entry;
                }
            }
        }
        else {
            /*
             * 'regular' modes: does this freelist entry have enough
             * allocated blocks to satisfy the request?
             *
             * Here we start looking for free space in the _oldest_
             * pages, so this classic system has the drawback of
             * increased 'risk' of finding free space near the START of
             * the file; given some pathological use cases, this means
             * we'll be scanning all/many freelist pages in about 50% fo
             * the searches (2 inserts, one delete at start, rinse & repeat -->
             * 1 insert at start + 1 insert at end),
             * resulting in a lot of page cache thrashing as the inserts
             * jump up and down the database; we can't help improve the
             * delete/erase operations in such cases, but we /can/ try
             * to keep the inserts close together.
             *
             * For that, you might be better served with the
             * convervative style of SEQUENTIAL above, as it will scan
             * freelist pages in reverse order.
             */
            if (hints->skip_init_offset)
            {
                start_index += hints->skip_init_offset;
                ham_assert(start_index >= 0, (0));
                /* only apply the init_offset at the first increment
                 * cycle to break repetitiveness */
                hints->skip_init_offset = 0;
            }
            else
            {
                start_index += hints->skip_step;
                ham_assert(start_index >= 0, (0));
            }

            /*
             * We don't have to be a very good SRNG here, so the 32-bit
             * int wrap around and the case where the result lands below
             * the 'start_index' limit are resolved in an (overly) simple
             * way:
             */
            start_index %= (freel_cache_get_count(cache) - hints->start_entry - hints->page_span_width + 1);
            start_index += hints->start_entry;
        }

        ham_assert(start_index < (ham_s32_t)freel_cache_get_count(cache), (0));
        ham_assert(start_index >= (ham_s32_t)hints->start_entry, (0));
        entry = freel_cache_get_entries(cache) + start_index;

        ham_assert(freel_entry_get_allocated_bits(entry)
                        <= freel_entry_get_max_bits(entry), (0));

        /*
         * the regular check: no way if there's not enough in there, lump sum
         */
        if (hints->page_span_width > 1) {
            /*
             * handle this a little differently for 'huge blobs' which span
             * multiple freelist entries: there, we'll be looking at _at
             * least_ SPAN-2 'fully allocated AND free' freelist entries,
             * that is: left edge (freelist entry), right edge entry and
             * zero or more 'full sized freelist entries' in between.
             *
             * Checking for these 'completely free' entries is much easier
             * (and faster) than plodding through their free bits to see
             * whether the requested number of free bits may be available.
             *
             * To keep it simple, we only check the first freelist entry
             * here and leave the rest to the outer search/alloc routine.
             *
             * NOTE: we 'shortcut' the SPAN-2 theoretical layout by aligning
             * such EXTREMELY HUGE BLOBS on a /freelist entry/ size boundary,
             * i.e. we consider such blobs to start at a fully free freelist
             * entry; consequently (thanks to this alignment, introduced
             * as a search optimization) such blobs take up SPAN-1 freelist
             * entries: no left edge, SPAN-1 full entries, right edge (i.e.
             * partial) freelist entry.
             *
             * This shortcut has a side effect: these extremely huge blobs
             * make the database storage space grow faster than absolutely
             * necessary when space efficiency would've been a prime concern:
             * as we 'align' such blobs to a freelist entry, we have a
             * worst-case fill rate of slighty over 50%: 1span+1chunk wide
             * blobs will 'span' 2 entries and is the smallest 'huge blob'
             * which will trigger this shortcut, resulting in it being
             * search-aligned to a fully free freelist entry every time,
             * meaning that we'll have a 'left over' of 1 /almost/ fully
             * free freelist entry per 'huge blob' --> fill
             * ratio = (1+.0000000001)/2 > 50%
             */
            if (freel_entry_get_allocated_bits(entry)
                    != freel_entry_get_max_bits(entry)) {
                continue;
            }
        }
        else {
            /*
             * regular requests do not overflow beyond the freelist entry
             * boundary, i.e. must fit in the current freelist entry page
             * in their entirety.
             */
            if (freel_entry_get_allocated_bits(entry) < hints->size_bits) {
                continue;
            }
        }

        /*
         * check our statistics as well: do we have a sufficiently
         * large chunk free in there?
         *
         * While we CANNOT say that we _know_ about the sizes of the
         * free slot zones available within the range first_start ..
         * last_start, we _do_ know how large the very last free chunk
         * is.
         *
         * Next to that, we also have a bit of a hunch about our level
         * of
         * 'utilization' (or 'fragmentation', depending on how you look
         * at it) of this middle range, so we can apply statistical
         * heuristics to this search: how certain do we want to be in
         * getting a hit in this freelist page?
         *
         * In FAST mode, we want to be dang sure indeed, so we simply
         * state that we want our slot taken out of that last chunk we
         * know all about, while the more conservative modes can improve
         * themselves with a bit of guesswork: when we had a lot of
         * FAILing trials, for instance, we might be best served by
         * accepting a little more sparseness in our storage here by
         * neglecting the midrange where free and filled slots mingle,
         * i.e. we SKIP that range then.
         *
         * More conservative, i.e. space saving folk would not have
         * this and demand we scan the lot, starting at the first free
         * bit in there.
         *
         * To aid this selection process, we call our hinter to give us
         * an (optimistic) estimate. Our current mgt_mode will take it
         * from there...
         */
        dst->startpos = 0;
        if (freel_entry_get_start_address(entry) < hints->lower_bound_address) {
            ham_assert(HAM_MAX_U32 >= ((hints->lower_bound_address
                - freel_entry_get_start_address(entry)) / DB_CHUNKSIZE), (0));
            dst->startpos = (ham_u32_t)((hints->lower_bound_address
                - freel_entry_get_start_address(entry)) / DB_CHUNKSIZE);
        }
        dst->endpos = freel_entry_get_max_bits(entry);
        dst->skip_distance = hints->size_bits;
        dst->mgt_mode = hints->mgt_mode;
        dst->aligned = hints->aligned;
        dst->lower_bound_address = hints->lower_bound_address;
        dst->size_bits = hints->size_bits;
        dst->freelist_pagesize_bits = hints->freelist_pagesize_bits;
        dst->page_span_width = hints->page_span_width;

        dst->cost = 1;

        if (hints->page_span_width > 1) {
            /*
             * with multi-entry spanning searches, there's no requirement
             * for per-page hinting, so we don't do it.
             *
             * However, we like our storage to be db page aligned, thank
             * you very much ;-)
             */
            dst->aligned = HAM_TRUE;
        }
        else {
            db_get_freelist_entry_hints(dst, device, env, entry);

            if (dst->startpos + dst->size_bits > dst->endpos) {
                /* forget it: not enough space in there anyway! */
                continue;
            }
        }

        /* we've done all we could to pick a good freelist page; now
         * it's up to the caller */
        break;
    }

    /* always count call as ONE round, at least: that's minus 1 for
     * the successful trial outside */
    hints->max_rounds--;

#if defined(HAM_DEBUG)
    ham_assert(start_index >= 0, (0));
    ham_assert(start_index < (ham_s32_t)freel_cache_get_count(cache), (0));
    ham_assert(start_index >= (ham_s32_t)hints->start_entry, (0));
    entry = freel_cache_get_entries(cache) + start_index;
    ham_assert(hints->page_span_width <= 1
                ? freel_entry_get_allocated_bits(entry) >= hints->size_bits
                : HAM_TRUE, (0));
    ham_assert(hints->page_span_width > 1
                ? freel_entry_get_allocated_bits(entry)
                        == freel_entry_get_max_bits(entry)
                : HAM_TRUE, (0));
#endif

    return start_index;
}


ham_status_t
__freel_alloc_area32(ham_offset_t *addr_ref, Device *device,
                Environment *env, Database *db, ham_size_t size,
                ham_bool_t aligned, ham_offset_t lower_bound_address)
{
    ham_status_t st;
    ham_s32_t i;
    freelist_entry_t *entry = NULL;
    freelist_payload_t *fp = NULL;
    freelist_cache_t *cache=device->get_freelist_cache();
    Page *page=0;
    ham_s32_t s=-1;
    ham_u16_t mgt_mode = db ? db->get_data_access_mode() : 0;
    freelist_global_hints_t global_hints =
    {
        0,
        1,
        0,
        freel_cache_get_count(cache),
        mgt_mode,
        0, /* span_width will be set by the hinter */
        aligned,
        lower_bound_address,
        size/DB_CHUNKSIZE,
        __freel_get_freelist_entry_maxspan(device, env, cache)
    };
    freelist_hints_t hints = {0};

    ham_assert(addr_ref != 0, (0));
    *addr_ref = 0;

    db_get_global_freelist_hints(&global_hints, device, env);

    ham_assert(!(env->get_flags()&HAM_IN_MEMORY_DB), (0));
    ham_assert(device->get_freelist_cache(), (0));

    ham_assert(size%DB_CHUNKSIZE==0, (0));

    ham_assert(global_hints.page_span_width >= 1, (0));
    /*
     * __freel_locate_sufficient_free_space() is used to calculate the
     * next freelist entry page to probe; as a side-effect it also
     * delivers the hints for this entry - no use calculating those a
     * second time for use in __freel_search_bits_ex() -- faster to pass
     * them along.
     */
    for (i = -1;
        0 <= (i = __freel_locate_sufficient_free_space(&hints,
                            &global_hints, device, env, cache, i));
        )
    {
        ham_assert(i < (ham_s32_t)freel_cache_get_count(cache), (0));

        entry = freel_cache_get_entries(cache) + i;
            
        /*
         * when we look for a free slot for a multipage spanning blob
         * ('huge blob'), we could, of course, play nice, and check every
         * bit of freelist, but that takes time.
         *
         * The faster approach employed here is to look for a sufficiently
         * large sequence of /completely free/ freelist pages; the worst
         * case space utilization of this speedup is >50% as the worst case
         * is looking for a chunk of space as large as one freelist page
         * (~ DB_CHUNKSIZE db pages) + 1 byte, in which case the second
         * freelist page will not be checked against a subsequent huge size
         * request as it is not 'completely free' any longer, thus effectively
         * occupying 2 freelist page spans where 1 (and a bit) would have
         * sufficed, resulting in a worst case space utilization of a little
         * more than 50%.
         *
         * I can live with that.
         */
        if (global_hints.page_span_width > 1) {
            /*
             * we must employ a different freelist alloc system for requests
             * spanning multiple freelist pages as the regular
             * __freel_search_bits_ex() is not able to cope with such
             * requests.
             *
             * hamsterdb versions prior to 1.1.0 would simply call that
             * function and fail every time, resulting in a behaviour where
             * 'huge blobs' could be added or overwritten in the database,
             * but erased huge blobs' space would never be re-used for
             * subsequently inserted 'huge blobs', thus resulting in an ever
             * growing database file when hamsterdb would be subjected to a
             * insert+erase use pattern for huge blobs.
             *
             * Note that the multipage spanning search employs a Boyer-Moore
             * search mechanism, which is (at least partly) built into the
             * __freel_locate_sufficient_free_space() function;
             * all that's left for us here is to scan _backwards_ conform
             * BM to see if we have a sufficiently large sequence of
             * completely freed freelist entries.
             */
            ham_size_t pagecount_sought = hints.page_span_width;
            ham_size_t start_idx;
            ham_size_t end_idx;
            ham_size_t available = freel_entry_get_allocated_bits(entry);

            ham_assert(freel_entry_get_allocated_bits(entry)
                        <= freel_entry_get_max_bits(entry), (0));
            if (i < (ham_s32_t)hints.page_span_width)
                return HAM_SUCCESS;
            ham_assert(i >= (ham_s32_t)hints.page_span_width, (0));
            /*
             * entry points at a freelist entry in the possible sequence, scan
             * back and forth to discover our actual sequence length. Scan
             * back first, then forward when we need a tail.
             */
            for (start_idx = 0; ++start_idx < pagecount_sought; ) {
                ham_assert(i >= (ham_s32_t)start_idx, (0));
                ham_assert(i - start_idx >= global_hints.start_entry, (0));
                if (freel_entry_get_allocated_bits(entry - start_idx)
                        != freel_entry_get_max_bits(entry - start_idx)) {
                    break;
                }
                available += freel_entry_get_allocated_bits(entry - start_idx);
            }
            start_idx--;

            /*
             * now see if we need (and have) a sufficiently large tail;
             * we can't simply say
             *
             *      pagecount_sought -= start_idx;
             *
             * because our very first freelist entry in the sequence may have
             * less maxbits than the others (as it may be the header page!)
             * so we need to properly calculate the number of freelist
             * entries that we need more:
             */
            ham_assert(hints.size_bits + hints.freelist_pagesize_bits - 1
                            >= available, (0));
            pagecount_sought = hints.size_bits - available;
            /* round up: */
            pagecount_sought += hints.freelist_pagesize_bits - 1;
            pagecount_sought /= hints.freelist_pagesize_bits;
            for (end_idx = 1;
                    end_idx < pagecount_sought
                    && i + end_idx < freel_cache_get_count(cache)
                    && (freel_entry_get_allocated_bits(entry + end_idx)
                        != freel_entry_get_max_bits(entry + end_idx));
                    end_idx++) {
                available += freel_entry_get_allocated_bits(entry + end_idx);
            }
            end_idx--;

            /*
             * we can move i forward to the first non-suitable entry and
             * BM-skip from there, HOWEVER, we have two BM modes in here
             * really: one that scans forward (DAM:RANDOM_ACCESS)
             * and one that scans backwards (DAM:SEQUENTIAL) and moving 'i'
             * _up_ would harm the latter.
             *
             * The way out of this is to add end_idx+1 as a skip_offset
             * instead and let __freel_locate_sufficient_free_space()
             * handle it from there.
             */
            global_hints.skip_init_offset = end_idx + 1;

            if (available < hints.size_bits) {
                /* register the NO HIT */
                db_update_freelist_globalhints_no_hit(device, env, entry, &hints);
            }
            else {
                ham_size_t len;
                ham_offset_t addr = 0;

                /* we have a hit! */
                i -= start_idx;
                end_idx += start_idx;
                
                start_idx = 0;
                for (len = hints.size_bits; len > 0; i++, start_idx++) {
                    ham_size_t fl;

                    ham_assert(i < (ham_s32_t)freel_cache_get_count(cache),(0));

                    entry = freel_cache_get_entries(cache) + i;
                    if (i == 0) {
                        page = 0;
                        fp = env->get_freelist();
                    }
                    else {
                        st = env_fetch_page(&page, env,
                                     freel_entry_get_page_id(entry), 0);
                        if (st)
                            return (st);
                        fp=page_get_freelist(page);
                    }
                    ham_assert(freel_entry_get_allocated_bits(entry)
                                == freel_entry_get_max_bits(entry), (0));
                    ham_assert(freel_get_allocated_bits32(fp)
                                == freel_get_max_bits32(fp), (0));

                    if (start_idx == 0) {
                        addr = freel_get_start_address(fp);
                    }

                    if (len >= freel_entry_get_allocated_bits(entry)) {
                        fl = freel_entry_get_allocated_bits(entry);
                    }
                    else {
                        fl = len;
                    }
                    __freel_set_bits(device, env, entry, fp,
                              HAM_FALSE, 0, fl, HAM_FALSE, &hints);
                    freel_set_allocated_bits32(fp,
                              (ham_u32_t)(freel_get_allocated_bits32(fp) - fl));
                    freel_entry_set_allocated_bits(entry,
                              freel_get_allocated_bits32(fp));
                    len -= fl;

                    if (page)
                        __page_set_dirty(page);
                    else
                        __env_set_dirty(env);
                }

                ham_assert(addr != 0, (0));
                *addr_ref = addr;
                return HAM_SUCCESS;
            }
        }
        else
        {
            /*
             * and this is the 'regular' free slot search, where we are
             * looking for sizes which fit into a single freelist entry page
             * in their entirety.
             *
             * Here we take the shortcut of not looking for edge solutions
             * spanning two freelist entries (start in one, last few chunks
             * in the next); this optimization costs little in space
             * utilization losses and gains us a lot in execution speed.
             * This particular optimization was already present in pre-v1.1.0
             * hamsterdb, BTW.
             */
            ham_assert(freel_entry_get_allocated_bits(entry)
                        >= size/DB_CHUNKSIZE, (0));
            ham_assert(hints.startpos + hints.size_bits
                        <= hints.endpos, (0));

            /*
             * yes, load the payload structure
             */
            if (i == 0) {
                fp = env->get_freelist();
            }
            else {
                st = env_fetch_page(&page, env,
                            freel_entry_get_page_id(entry), 0);
                if (st)
                    return (st);
                fp=page_get_freelist(page);
            }

            /*
             * now try to allocate from this payload
             */
            s = __freel_search_bits_ex(device, env, entry, fp,
                            size/DB_CHUNKSIZE, &hints);
            if (s != -1)
            {
                __freel_set_bits(device, env, entry, fp, HAM_FALSE,
                            s, size/DB_CHUNKSIZE, HAM_FALSE, &hints);
                if (page)
                    __page_set_dirty(page);
                else
                    __env_set_dirty(env);
                break;
            }
            else
            {
                /* register the NO HIT */
                db_update_freelist_globalhints_no_hit(device, env, entry, &hints);
            }
        }
    }

    ham_assert(s != -1 ? fp != NULL : 1, (0));

    if (s != -1)
    {
        freel_set_allocated_bits32(fp,
                (ham_u32_t)(freel_get_allocated_bits32(fp)-size/DB_CHUNKSIZE));
        freel_entry_set_allocated_bits(entry,
                freel_get_allocated_bits32(fp));

        *addr_ref = (freel_get_start_address(fp)+(s*DB_CHUNKSIZE));
        return HAM_SUCCESS;
    }

    *addr_ref = 0;
    return HAM_SUCCESS;
}

ham_status_t
__freel_lazy_create32(freelist_cache_t *cache, Device *device,
                Environment *env)
{
    ham_status_t st;
    ham_size_t size;
    ham_size_t entry_pos;
    freelist_entry_t *entry;
    freelist_payload_t *fp=env->get_freelist();
    
    ham_assert(device->get_freelist_cache() == 0, (0));
    ham_assert(cache != 0, (0));
    ham_assert(!freel_cache_get_entries(cache), (0));

    entry=(freelist_entry_t *)env->get_allocator()->calloc(sizeof(*entry)*1);
    if (!entry)
        return HAM_OUT_OF_MEMORY;

    /*
     * add the header page to the freelist
     */
    freel_entry_set_start_address(&entry[0], env->get_pagesize());
    size = env->get_usable_pagesize();
    size -= SIZEOF_FULL_HEADER(env);
    size -= db_get_freelist_header_size32();
    size -= size % sizeof(ham_u64_t);

    ham_assert((size % sizeof(ham_u64_t)) == 0, ("freelist entry bitarray == 0 MOD sizeof(ham_u64_t) due to the scan algorithm"));
    freel_entry_set_max_bits(&entry[0], (ham_u32_t)(size*8));

    /*
     * initialize the header page, if we have read/write access
     */
    if (!(env->get_flags()&HAM_READ_ONLY))
    {
        freel_set_start_address(fp, env->get_pagesize());
        ham_assert((size*8 % sizeof(ham_u64_t)) == 0, ("freelist bitarray size must be == 0 MOD sizeof(ham_u64_t) due to the scan algorithm"));
        freel_set_max_bits32(fp, (ham_u32_t)(size*8));
    }

    ham_assert(cache->_init_perf_data, (0));
    st = cache->_init_perf_data(cache, device, env, entry, fp);
    if (st) {
        return st;
    }

    freel_cache_set_count(cache, 1);
    freel_cache_set_entries(cache, entry);
       
    ham_assert(device, (0));
    device->set_freelist_cache(cache);
    ham_assert(device->get_freelist_cache() != 0, (0));

    /*
     * now load all other freelist pages
     */
    for (entry_pos = 1;; entry_pos++)
    {
        Page *page;
        if (!freel_get_overflow(fp))
            break;

        st=__freel_cache_resize(device, env, cache, freel_cache_get_count(cache)+1);
        if (st)
            return st;

        st=env_fetch_page(&page, env, freel_get_overflow(fp), 0);
        if (st)
            return (st);

        fp=page_get_freelist(page);
        ham_assert(entry_pos<freel_cache_get_count(cache), (0));
        entry=freel_cache_get_entries(cache)+entry_pos;
        ham_assert(freel_entry_get_start_address(entry) == freel_get_start_address(fp), (0));
        freel_entry_set_allocated_bits(entry, freel_get_allocated_bits32(fp));
        freel_entry_set_page_id(entry, page->get_self());

        ham_assert(cache->_init_perf_data, (0));
        st = cache->_init_perf_data(cache, device, env, entry, fp);
        if (st) {
            return st;
        }
    }

    return (0);
}

ham_status_t
__freel_destructor32(Device *device, Environment *env)
{
    freelist_cache_t *cache;
    freelist_entry_t *entries;

    ham_assert(!(env->get_flags()&HAM_IN_MEMORY_DB), (0));
    ham_assert(device->get_freelist_cache(), (0));

    cache=device->get_freelist_cache();
    ham_assert(cache, (0));

    entries = freel_cache_get_entries(cache);
    if (entries)
        env->get_allocator()->free(entries);

    memset(cache, 0, sizeof(*cache));

    return (0);
}


ham_status_t
__freel_mark_free32(Device *device, Environment *env, Database *db,
                ham_offset_t address, ham_size_t size, ham_bool_t overwrite)
{
    ham_status_t st;
    Page *page=0;
    freelist_cache_t *cache;
    freelist_entry_t *entry;
    freelist_payload_t *fp;
    ham_size_t s;
    freelist_hints_t hints =
    {
    0,
    0,
    0,
    db ? db->get_data_access_mode() : 0, /* mgt_mode */
    HAM_FALSE,
    0,
    0,
    0,
    0,
    0
    };

    ham_assert(!(env->get_flags()&HAM_IN_MEMORY_DB), (0));
    ham_assert(device->get_freelist_cache(), (0));

    ham_assert(size%DB_CHUNKSIZE==0, (0));
    ham_assert(address%DB_CHUNKSIZE==0, (0));

    cache = device->get_freelist_cache();
    ham_assert(cache, (0));

    /*
     * split the chunk if it doesn't fit in one freelist page
     */
    while (size)
    {
        /*
         * get the cache entry of this address
         */
        st=__freel_cache_get_entry(&entry, device, env, cache, address);
        /* [i_a] old code had a subtle out-of-mem crash failure which would not be caught as the error was not checked before 'entry' was used */
        ham_assert(st ? entry == NULL : entry != NULL, (0));
        if (st)
            return st;

        /*
         * allocate a page if necessary
         */
        if (!freel_entry_get_page_id(entry)) {
            if (freel_entry_get_start_address(entry)==env->get_pagesize()) {
                fp=env->get_freelist();
                ham_assert(freel_get_start_address(fp) != 0, (0));
            }
            else {
                st=__freel_alloc_page32(&page, device, env, cache, entry);
                if (st)
                    return (st);
                fp=page_get_freelist(page);
                ham_assert(freel_get_start_address(fp) != 0, (0));
            }
        }
        /*
         * otherwise just fetch the page from the cache or the disk
         */
        else {
            st=env_fetch_page(&page, env, freel_entry_get_page_id(entry), 0);
            if (st)
                return (st);
            fp=page_get_freelist(page);
            ham_assert(freel_get_start_address(fp) != 0, (0));
        }

        ham_assert(address>=freel_get_start_address(fp), (0));

        /*
         * set the bits and update the values in the cache and
         * the fp
         */
        s = __freel_set_bits(device, env, entry, fp, overwrite,
                (ham_size_t)(address-freel_get_start_address(fp))
                        / DB_CHUNKSIZE,
                size/DB_CHUNKSIZE,
                HAM_TRUE, &hints);

        freel_set_allocated_bits32(fp,
                (ham_u32_t)(freel_get_allocated_bits32(fp)+s));
        freel_entry_set_allocated_bits(entry,
                freel_get_allocated_bits32(fp));

        if (page)
            __page_set_dirty(page);
        else
            __env_set_dirty(env);

        size -= s * DB_CHUNKSIZE;
        address += s * DB_CHUNKSIZE;
    }

    return HAM_SUCCESS;
}


ham_status_t
__freel_check_area_is_allocated32(Device *device, Environment *env, ham_offset_t address, ham_size_t size)
{
    ham_status_t st;
    Page *page=0;
    freelist_cache_t *cache;
    freelist_entry_t *entry;
    freelist_payload_t *fp;
    ham_size_t s;

    ham_assert(!(env->get_flags()&HAM_IN_MEMORY_DB), (0));

    ham_assert(size%DB_CHUNKSIZE==0, (0));
    ham_assert(address%DB_CHUNKSIZE==0, (0));

    cache = device->get_freelist_cache();
    ham_assert(cache, (0));

    /*
    * split the chunk if it doesn't fit in one freelist page
    */
    while (size)
    {
        /*
        * get the cache entry of this address
        */
        st=__freel_cache_get_entry(&entry, device, env, cache, address);
        /* [i_a] old code had a subtle out-of-mem crash failure which would not be caught as the error was not checked before 'entry' was used */
        ham_assert(st ? entry == NULL : entry != NULL, (0));
        if (st)
            return st;

        /*
        * allocate a page if necessary
        */
        if (!freel_entry_get_page_id(entry)) {
            if (freel_entry_get_start_address(entry)==env->get_pagesize()) {
                fp=env->get_freelist();
                ham_assert(freel_get_start_address(fp) != 0, (0));
            }
            else {
                st=__freel_alloc_page32(&page, device, env, cache, entry);
                if (st)
                    return (st);
                fp=page_get_freelist(page);
                ham_assert(freel_get_start_address(fp) != 0, (0));
            }
        }
        /*
        * otherwise just fetch the page from the cache or the disk
        */
        else {
            st=env_fetch_page(&page, env, freel_entry_get_page_id(entry), 0);
            if (st)
                return (st);
            fp=page_get_freelist(page);
            ham_assert(freel_get_start_address(fp) != 0, (0));
        }

        ham_assert(address>=freel_get_start_address(fp), (0));

        /*
        * check the bits
        */
        s=size;
        size-=s;
        address+=s;
    }

    return HAM_SUCCESS;
}

/**
 * setup / initialize the proper performance data for this freelist
 * page.
 *
 * Yes, this data will (very probably) be lost once the page has been
 * removed from the in-memory cache, unless the currently active
 * freelist algorithm persists this data to disc.
 */
ham_status_t
__freel_init_perf_data32(freelist_cache_t *cache, Device *device, Environment *env,
        freelist_entry_t *entry, freelist_payload_t *fp)
{
    freelist_page_statistics_t *entrystats = freel_entry_get_statistics(entry);

    /* we can assume all freelist FP data has been zeroed before we came
     * in here */

    if (fp && entrystats->persisted_bits == 0)
    {
        ham_status_t st;
        ham_offset_t filesize;
        Device *device;

        /*
         * now comes the hard part: when we don't have overflow, we
         * know the ACTUAL end is in here somewhere, but definitely not
         * at _max_bits.
         *
         * So we take the fastest road towards establishing the end: we
         * request the file size and calculate how many chunks that
         * would be and consequently how many chunks are in this
         * [section of the] freelist.
         */
        device = env->get_device();
        ham_assert(device, (0));
        st = device->get_filesize(&filesize);
        if (st)
            return st;

        ham_assert(filesize > 0, (0));
        if (filesize > fp->_start_address)
        {
            filesize -= fp->_start_address;
            filesize /= DB_CHUNKSIZE;
            if (filesize > fp->_s._s32._max_bits) {
                /* can happen when something (blob/test) causes an
                 * allocation of multiple pages at once */
                filesize = fp->_s._s32._max_bits;
            }
        }
        else
        {
            /* overflow */
            filesize = 0;
        }

        entrystats->persisted_bits = (ham_u32_t)filesize;
    }

    return HAM_SUCCESS;
}
                                                                    
/* ------------------------------------------------ */

static ham_status_t
__freel_constructor(Device *device, Environment *env, Database *db)
{
    ham_status_t st;
    freelist_cache_t *cache;
    
    ham_assert(device->get_freelist_cache()==0, (0));

    ham_assert(env->get_header_page(), (0));
    ham_assert(env->get_header(), (0));
    //ham_assert(env_get_data_access_mode(env) == 0, (0));

    st = freel_constructor_prepare32(&cache, device, env);
    ham_assert(cache ? !st : 1, (0));
    ham_assert(cache ? 1 : st, (0));

    if (cache)
    {
        //cache = device_get_freelist_cache(device);
        ham_assert(cache, (0));
        ham_assert(cache->_constructor != 0, (0));
        st = cache->_constructor(cache, device, env);
    }

    ham_assert(st ? 1 : device->get_freelist_cache()!=0, (0));
    ham_assert(st ? 1 : device->get_freelist_cache()->_alloc_area != 0, (0));

    return (st);
}

ham_status_t
freel_constructor_prepare32(freelist_cache_t **cache_ref, Device *device,
                Environment *env)
{
    //ham_status_t st;
    freelist_cache_t *cache;
    
    ham_assert(device->get_freelist_cache()==0, (0));

    *cache_ref = 0;

    cache = (freelist_cache_t *)env->get_allocator()->calloc(sizeof(*cache));
    if (!cache)
        return (HAM_OUT_OF_MEMORY);

    ham_assert(env->get_header_page(), (0));
    ham_assert(env->get_header(), (0));
    //ham_assert(env_get_data_access_mode(env) == 0, (0));

    cache->_constructor = __freel_lazy_create32;
    cache->_destructor = __freel_destructor32;
    cache->_flush_stats = __freel_flush_stats32;
    cache->_alloc_area = __freel_alloc_area32;
    cache->_mark_free = __freel_mark_free32;
    cache->_check_area_is_allocated = __freel_check_area_is_allocated32;
    cache->_init_perf_data = __freel_init_perf_data32;

    //st = cache->_constructor(cache, device, env, env_get_data_access_mode(env));
    //
    //ham_assert(st ? 1 : device->get_freelist_cache()!=0, (0));
    //ham_assert(st ? 1 : device->get_freelist_cache()->_alloc_area != 0, (0));
    //
    //return (st);

    *cache_ref = cache;
    return HAM_SUCCESS;
}

ham_status_t
freel_shutdown(Environment *env)
{
    freelist_cache_t *cache;
    Device *device;
    ham_status_t st;

    if (env->get_flags()&HAM_IN_MEMORY_DB)
        return (0);

    /*
     * if there's no device then most likely we're closing an Environment
     * which was not fully initialized - that's ok, no need to return
     * an error
     */
    device=env->get_device();
    if (!device)
        return (0);

    cache=device->get_freelist_cache();
    if (!cache)
        return (0);

    ham_assert(cache->_flush_stats, (0));
    st = cache->_flush_stats(device, env);

    ham_assert(cache->_destructor, (0));
    st = cache->_destructor(device, env);

    env->get_allocator()->free(cache);
    if (env)
        device->set_freelist_cache(0);
    else
        device->set_freelist_cache(0);

    return (st);
}

ham_status_t
freel_mark_free(Environment *env, Database *db, ham_offset_t address,
                ham_size_t size, ham_bool_t overwrite)
{
    freelist_cache_t *cache;
    Device *device;
    ham_status_t st;

    if (env->get_flags()&HAM_IN_MEMORY_DB)
        return (0);

    ham_assert(size%DB_CHUNKSIZE==0, (0));
    ham_assert(address%DB_CHUNKSIZE==0, (0));

    device = env->get_device();

    if (!device->get_freelist_cache()) {
        st = __freel_constructor(env->get_device(), env, db);
        if (st)
            return st;
    }
    cache=device->get_freelist_cache();

    ham_assert(cache, (0));
    ham_assert(cache->_mark_free, (0));
    st = cache->_mark_free(device, env, db, address, size, overwrite);

    return (st);
}

ham_status_t
freel_check_area_is_allocated(Environment *env, Database *db,
                ham_offset_t address, ham_size_t size)
{
    freelist_cache_t *cache;
    Device *device;
    ham_status_t st;

    if (env->get_flags()&HAM_IN_MEMORY_DB)
        return HAM_SUCCESS;

    ham_assert(size%DB_CHUNKSIZE==0, (0));
    ham_assert(address%DB_CHUNKSIZE==0, (0));

    device=env->get_device();

    if (!device->get_freelist_cache()) {
        st = __freel_constructor(env->get_device(), env, db);
        if (st)
            return st;
    }
    cache=device->get_freelist_cache();

    ham_assert(cache, (0));
    ham_assert(cache->_check_area_is_allocated, (0));
    st = cache->_check_area_is_allocated(device, env, address, size);

    return st;
}

ham_status_t
freel_alloc_area(ham_offset_t *addr_ref, Environment *env, Database *db,
                ham_size_t size)
{
    return freel_alloc_area_ex(addr_ref, env, db, size, HAM_FALSE, 0);
}

ham_status_t
freel_alloc_area_ex(ham_offset_t *addr_ref, Environment *env, Database *db,
                ham_size_t size, ham_bool_t aligned,
                ham_offset_t lower_bound_address)
{
    freelist_cache_t *cache;
    Device *device;

    ham_assert(addr_ref != 0, (0));
    *addr_ref = 0;

    if (env->get_flags()&HAM_IN_MEMORY_DB)
    {
        return HAM_SUCCESS;
    }

    device=env->get_device();

    if (!device->get_freelist_cache())
    {
        ham_status_t st = __freel_constructor(env->get_device(), env, db);
        if (st)
        {
            return st;
        }
    }
    cache=device->get_freelist_cache();

    ham_assert(cache, (0));
    ham_assert(cache->_alloc_area, (0));
    return cache->_alloc_area(addr_ref, env->get_device(), env, db,
                size, aligned, lower_bound_address);
}

ham_status_t
freel_alloc_page(ham_offset_t *addr_ref, Environment *env, Database *db)
{
    ham_status_t st;
    freelist_cache_t *cache;
    Device *device;

    *addr_ref = 0;
    if (env->get_flags()&HAM_IN_MEMORY_DB)
        return HAM_SUCCESS;

    device=env->get_device();

    if (!device->get_freelist_cache()) {
        ham_status_t st = __freel_constructor(env->get_device(), env, db);
        if (st)
            return st;
    }
    cache=device->get_freelist_cache();

    ham_assert(cache, (0));
    ham_assert(cache->_alloc_area, (0));
    st = cache->_alloc_area(addr_ref, env->get_device(), env, db,
                env->get_pagesize(), HAM_TRUE, 0);
    return st;
}

#define DUMMY_LSN  1

ham_status_t
__freel_flush_stats32(Device *device, Environment *env)
{
    ham_status_t st;

    ham_assert(!(env->get_flags()&HAM_IN_MEMORY_DB), (0));
    ham_assert(device->get_freelist_cache(), (0));

    /*
     * do not update the statistics in a READ ONLY database!
     */
    if (!(env->get_flags() & HAM_READ_ONLY)) {
        freelist_cache_t *cache;
        freelist_entry_t *entries;

        cache=device->get_freelist_cache();
        ham_assert(cache, (0));

        entries = freel_cache_get_entries(cache);

        if (entries && freel_cache_get_count(cache) > 0) {
            /*
             * only persist the statistics when we're using a v1.1.0+ format DB 
             *
             * if freelist_v2 is used, the file is always 1.1.+ format.
             */
            ham_size_t i;
            
            for (i = freel_cache_get_count(cache); i-- > 0; ) {
                freelist_entry_t *entry = entries + i;

                if (freel_entry_statistics_is_dirty(entry)) {
                    freelist_payload_t *fp;
                    freelist_page_statistics_t *pers_stats;

                    if (!freel_entry_get_page_id(entry)) {
                        /* header page */
                        fp = env->get_freelist();
                        env->set_dirty(true);
                    }
                    else {
                        /*
                         * otherwise just fetch the page from the cache or the 
                         * disk
                         */
                        Page *page;
                        
                        st = env_fetch_page(&page, env,
                                freel_entry_get_page_id(entry), 0);
                        if (st)
                            return (st);
                        fp = page_get_freelist(page);
                        ham_assert(freel_get_start_address(fp) != 0, (0));
                        page->set_dirty(true);
                    }

                    ham_assert(fp->_s._s32._zero == 0, (0));

                    pers_stats = freel_get_statistics32(fp);

                    ham_assert(sizeof(*pers_stats) == 
                            sizeof(*freel_entry_get_statistics(entry)), (0));
                    memcpy(pers_stats, freel_entry_get_statistics(entry), 
                            sizeof(*pers_stats));

                    /* and we're done persisting/flushing this entry */
                    freel_entry_statistics_reset_dirty(entry);
                }
            }
        }
    }

    if (env->get_flags()&HAM_ENABLE_RECOVERY)
        return (env->get_changeset().flush(DUMMY_LSN));

    env->get_changeset().clear();

    return (0);
}


