
/*--------------------------------------------------------------------*/
/*--- Helgrind: a Valgrind tool for detecting errors               ---*/
/*--- in threaded programs.                              hg_main.c ---*/
/*--------------------------------------------------------------------*/

/*
   This file is part of Helgrind, a Valgrind tool for detecting errors
   in threaded programs.

   Copyright (C) 2007-2008 OpenWorks LLP
      info@open-works.co.uk

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307, USA.

   The GNU General Public License is contained in the file COPYING.

   Neither the names of the U.S. Department of Energy nor the
   University of California nor the names of its contributors may be
   used to endorse or promote products derived from this software
   without prior written permission.
*/

/* JRS: TODO 2008 Mar 09:

   - Consider what to do about BHL all over again

   - Consider what to do about last-lock-lossage mechanism.
     Should it be removed?

   - Have something like mk_SHVAL_fail instead of merely asserting

   - More comments re deletion of memory containing Locks.

     New proposal: Locks are never deleted, and entries never removed
     from map_locks.  Instead have a .dormant bit in Lock indicating
     that they are have been freed by the user.

     (Later): remove map_locks.  Instead, on each SecMap have a set of
     Locks which are known to live, or have lived, in that SecMap
     (that is, their GA is in that SecMap).

     This effectively means that we allocate one Lock entry for every
     address that the client uses for a lock, and remember it forever.

   - Fix command line ordering assumptions for --ignore-i= vs --ignore-n=

   STUFF I DON'T UNDERSTAND:

   Make sense of ignore-n/ignore-i.  What exactly does this do?
   Why shift by the secmap size before modding?

   How does the pthread_barrier_wait wrapper create the correct
   segments?  In particular, why does it use the same client requests
   as for pthread_cond_* operations?
*/

#include "pub_tool_basics.h"
#include "pub_tool_aspacemgr.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_libcbase.h"
#include "pub_tool_libcprint.h"
#include "pub_tool_libcproc.h"
#include "pub_tool_mallocfree.h"
#include "pub_tool_threadstate.h"
#include "pub_tool_tooliface.h"
#include "pub_tool_hashtable.h"
#include "pub_tool_replacemalloc.h"
#include "pub_tool_machine.h"
#include "pub_tool_options.h"
#include "pub_tool_xarray.h"
#include "pub_tool_stacktrace.h"
#include "pub_tool_debuginfo.h"  /* VG_(get_data_description) */

#include "helgrind.h"

#define HG_(str) VGAPPEND(vgHelgrind_,str)
#include "hg_debugonly.h"
#include "hg_wordfm.h"
#include "hg_wordset.h"

static void pp_memory_usage ( Int flags, Char* caller );
static void pp_mem_laog ( Int d );
void pp_fake_segment_stats ( Int d );
static void pp_stats ( Char * caller );
static void hg_reset_stats ( void );

/*----------------------------------------------------------------*/
/*--- Documentation on CacheLine/CacheLineZ/CacheLineF stuff   ---*/
/*----------------------------------------------------------------*/

/* This comment describes how the shadow value cache and compressed
   shadow value representation works.  Note that all line sizes etc
   are defined by constants as shown; the actual numbers shown are
   merely indicate the current settings.

   Outside the cache, SVals are stored in a compressed representation
   ------------------------------------------------------------------

   Top level of the representation is "map_shmem", which is a WordFM
   from Addr to SecMap*.  Each SecMap* holds SVals for N_SECMAP_ARANGE
   bytes of address range (naturally aligned, of course).  The Addr in
   the WordFM is the lowest guest address in the SecMap.

   SecMaps are never deleted; once created they last forever.

   To find a secmap call shmem_find_SecMap.  This basically does
   lookupFM in map_shmem; except it uses a tiny 3 element cache which
   cuts out about 95% of the lookupFMs.

   Currently each SecMap covers N_SECMAP_ARANGE (2^13, 8192) bytes of
   address space.

   A SecMap contains an array of CacheLineZ.  (Z = compressed, F =
   full).  Each CacheLineZ covers N_LINE_ARANGE (curr. 64) bytes of
   address space, so currently each SecMap contains 128 CacheLineZs.

   A CacheLineZ holds 64 SVals in the common case where there are only
   4 or fewer different values.  In this case, the SVals are stored in
   .dict[0 .. 3], and .ix2s[] is an array of 64 2-bit indexes into
   dict[], once for each of the 64 SVals.  This gives a compression
   ratio of (64 * sizeof(SVal)) : (4 * sizeof(SVal) + 64 * 0.25),
   which is 512:48, or 10.67:1.

   In 0%-20% of the lines, each 64-group of SVals has more than 4
   different values.  In that case the group is stored uncompressed in
   a CacheLineF (full representation).  CacheLineF is just an array of
   64 SVals + an "in use" Boolean.

   Each SecMap has an attached array of CacheLineFs (.linesF,
   .linesF_size).  When data will not fit in a CacheLineZ, the
   SecMap's CacheLineF array is searched to find a free entry (.inUse
   == False).  The CacheLineZ's dict[0] entry is set to zero (an
   impossible SVal) to indicate that the data is really in a
   CacheLineF.  The dict[1] gives the index in .linesF of the
   CacheLineF.  (This is why it is important that no valid SVal has
   value zero).

   Inside the cache
   ----------------

   The SVal cache is an array of N_WAY_NENT (2^16) entries organised
   as a direct mapped cache.  Each entry covers 64 bytes of address
   space, so that each CacheLine exactly covers one CacheLineZ/F in
   the outside-of-cache representation.

   Lines are moved into the cache by cacheline_fetch and out by
   cacheline_wback.  These decompress/compress respectively.  Inside
   the cache, the SVals for each 8 bytes of address space are referred
   to as a "tree".  For each 8 bytes of address space, there are 8
   SVals in CacheLine.svals[], and one 16-bit descriptor for the 8
   SVals.  The descriptor indicates which of the 8 SVals are valid
   right now.  For example, if all of the 8 SVals are the same, then
   the descriptor is TREE_DESCR_64, which indicates that only the
   first SVal in the group of 8 is valid.  This means it is possible
   to do a single msm_handle_{read,write} call and get a new value for
   the implied 8 SVals.

   If it is necessary to split a 64 bit group (say, into 2 32-bit
   pieces) because the two pieces require different SVals, then the
   tree's [0] entry is copied to [0] and [4] and the tree's descriptor
   is changed to (TREE_DESCR_32_0 | TREE_DESCR_32_1).  This is called
   a "pulldown" and it means that subsequent msm_handle_{read,write}
   calls have to be done for each 32-bit chunk individually.  Values
   can get pulled down to 16- and 8-bit granularity if needed.

   The point of this complexity is to minimise the number of calls to
   msm_handle_{read,write} by doing them at the highest level of
   granularity (up to 8-byte) that is possible.  So it supports doing
   byte level shadow values if necessary whilst being efficient in the
   common case where 32/64-bit accesses happen and all 4/8 SVals in
   the range are the same.

   At the same time, by using a SVal cache, these large (64-bit per
   byte) SVals can be stored in a relatively space efficient way.
*/

/*----------------------------------------------------------------*/
/*--- Preamble                                                 ---*/
/*----------------------------------------------------------------*/

/* Note this needs to be compiled with -fno-strict-aliasing, since it
   contains a whole bunch of calls to lookupFM etc which cast between
   Word and pointer types.  gcc rightly complains this breaks ANSI C
   strict aliasing rules, at -O2.  No complaints at -O, but -O2 gives
   worthwhile performance benefits over -O.
*/

// FIXME catch sync signals (SEGV, basically) and unlock BHL,
// if held.  Otherwise a LOCK-prefixed insn which segfaults 
// gets Helgrind into a total muddle as the BHL will not be
// released after the insn.

// FIXME what is supposed to happen to locks in memory which
// is relocated as a result of client realloc?

// FIXME accesses to NoAccess areas: change state to Excl?

// FIXME report errors for accesses of NoAccess memory?

// FIXME pth_cond_wait/timedwait wrappers.  Even if these fail,
// the thread still holds the lock.

/* ------------ Debug/trace options ------------ */

// this is:
// shadow_mem_make_NoAccess: 29156 SMs, 1728 scanned
// happens_before_wrk: 1000
// ev__post_thread_join: 3360 SMs, 29 scanned, 252 re-Excls
#define SHOW_EXPENSIVE_STUFF 0

// 0 for silent, 1 for some stuff, 2 for lots of stuff
#define SHOW_EVENTS 0

// Flags for controlling for which events sanity checking is done
#define SCE_THREADS  (1<<0)  // Sanity check at thread create/join
#define SCE_LOCKS    (1<<1)  // Sanity check at lock events
#define SCE_BIGRANGE (1<<2)  // Sanity check at big mem range events
#define SCE_ACCESS   (1<<3)  // Sanity check at mem accesses
#define SCE_LAOG     (1<<4)  // Sanity check at significant LAOG events
#define SCE_HBEFORE  (1<<5)  // Crosscheck VTS vs Explicit h-before-graph

#define SCE_BIGRANGE_T 256  // big mem range minimum size


/* For the shadow mem cache stuff we may want more intrusive
   checks.  Unfortunately there's no almost-zero-cost way to make them
   selectable at run time.  Hence set the #if 0 to #if 1 and
   rebuild if you want them. */
#if 0
#  define SCE_CACHELINE 1  /* do sanity-check CacheLine stuff */
#  define inline __attribute__((noinline))
   /* probably want to ditch -fomit-frame-pointer too */
#else
#  define SCE_CACHELINE 0   /* don't sanity-check CacheLine stuff */
#endif

/* For the SegmentID, SegmentSet and SVal stuff we may want more
   intrusive checks.  Again there's no zero cost way to do this.  Set
   the #if 0 to #if 1 and rebuild if you want them. */
#if 0
#  define SCE_SVALS 1 /* sanity-check shadow value stuff */
#else
#  define SCE_SVALS 0
#endif

static void all__sanity_check ( Char* who ); /* fwds */

#define HG_CLI__MALLOC_REDZONE_SZB 16 /* let's say */

// 0 for none, 1 for dump at end of run
#define SHOW_DATA_STRUCTURES 0


/* ------------ Command line options ------------ */

// 0 = no segments at all
// 1 = segments at thread create/join
// 2 = as 1 + segments at condition variable signal/broadcast/wait
//          + segments at sem_wait/sem_post
static Int clo_happens_before = 2;  /* default setting */

/* Generate .vcg output of the happens-before graph?
   0: no  1: yes, without VTSs  2: yes, with VTSs */
static Int clo_gen_vcg = 0;

/* When comparing race errors for equality, should the race address be
   taken into account?  For users, no, but for verification purposes
   (regtesting) this is sometimes important. */
static Bool clo_cmp_race_err_addrs = False;

/* Tracing memory accesses, so we can see what's going on.
   clo_trace_addr is the address to monitor.  clo_trace_level = 0 for
   no tracing, 1 for summary, 2 for detailed. */
static Addr clo_trace_addr  = 0;
static Int  clo_trace_level = 0;

/* Sanity check level.  This is an or-ing of
   SCE_{THREADS,LOCKS,BIGRANGE,ACCESS,LAOG}. */
static Int clo_sanity_flags = 0;

/* If clo_ignore_n == 0, the state machine ignores all memory addresses. 
   If clo_ignore_n >= 2, the addresses are ignored if 
           (addr mod clo_ignore_n != clo_ignore_i)
  
  TODO: Find a better (descriptive) name for command line parameters. */
static UWord clo_ignore_n = 1;
static UWord clo_ignore_i = 0;


/* Max number of segments in a segment set. 
   The default value is large enough, but allows to stay sane.
   Must be >= 4.
 */
static UInt clo_max_segment_set_size = 20;


// If true, segment set recycling is enabled. 
static Bool clo_ss_recycle = True;

// If true, we record the contexts of 
//   - last lock lossage and 
//   - segment creation
static Bool clo_more_context = True;

// If true, Helgrind starts behaving almost like 
// a pure happens-before detector (i.e. it creates a happens-before
// arc between each matching Unlock and Lock operations). 
// Very time and memory consuming! 
static Bool clo_pure_happens_before = False;

// If true, all races inside a C++ destructor will be ignored. 
static Bool clo_ignore_in_dtor = False;

// Print no more than this number of traces after a race has been detected.
static UInt clo_trace_after_race = 50;


// Size of context for locks. Usually should be less than --num-callers 
// since collecting context for locks is quite expensive. 
static UInt clo_num_callers_for_locks = 9;


/* This has to do with printing error messages.  See comments on
   announce_threadset() and summarise_threadset().  Perhaps it
   should be a command line option. */
#define N_THREADS_TO_ANNOUNCE 5


/* ------------ Misc comments ------------ */

// FIXME: don't hardwire initial entries for root thread.
// Instead, let the pre_thread_ll_create handler do this.

// FIXME: when a SecMap is completely set via and address range
// setting operation to a non-ShR/M state, clear its .mbHasShared 
// bit

/* FIXME: figure out what the real rules are for Excl->ShR/M
   transitions w.r.t locksets.

   Muelenfeld thesis Sec 2.2.1 p 8/9 says that

     When another thread accesses the memory location, the lock-set
     is initialized with all active locks and the algorithm reports
     the next access that results in an empty lock-set.

   What does "all active locks" mean?  All locks held by the accessing
   thread, or all locks held by the system as a whole?

   However: Muelenfeld's enhanced Helgrind (eraser_mem_read_word)
   seems to use simply the set of locks held by the thread causing the
   transition into a shared state at the time of the transition:

     *sword = SW(Vge_Shar, packLockSet(thread_locks_rd[tid]));

   Original Eraser paper also says "all active locks".
*/

/* Thread async exit:
   
   remove the map_threads entry
   leave the Thread object in place
   complain if holds any locks
   
   unlike with Join, do not change any memory states

   I _think_ this is correctly handled now.
*/

/*----------------------------------------------------------------*/
/*--- Some very basic stuff                                    ---*/
/*----------------------------------------------------------------*/

static UWord stat__hg_zalloc = 0;
static UWord stat__hg_free = 0;

static void* hg_zalloc ( SizeT n ) {
   void* p;
   tl_assert(n > 0);
   p = VG_(malloc)( n );
   tl_assert(p);
   VG_(memset)(p, 0, n);
   stat__hg_zalloc++;
   return p;
}
static void hg_free ( void* p ) {
   tl_assert(p);
   VG_(free)(p);
   stat__hg_free++;
}

/* Round a up to the next multiple of N.  N must be a power of 2 */
#define ROUNDUP(a, N)   ((a + N - 1) & ~(N-1))
/* Round a down to the next multiple of N.  N must be a power of 2 */
#define ROUNDDN(a, N)   ((a) & ~(N-1))

#ifdef HAVE_BUILTIN_EXPECT
#define LIKELY(cond)   __builtin_expect(!!(cond),1)
#define UNLIKELY(cond) __builtin_expect(!!(cond),0)
#else
#define LIKELY(cond)   (cond)
#define UNLIKELY(cond) (cond)
#endif

// Paranoia:  it's critical for performance that the requested inlining
// occurs.  So try extra hard.
#define INLINE    inline __attribute__((always_inline))


/*----------------------------------------------------------------*/
/*--- Primary data definitions                                 ---*/
/*----------------------------------------------------------------*/

/* Shadow values. */
typedef  ULong  SVal;


/* These are handles for thread segments.  CONSTRAINTS: Must be small
   ints numbered from zero, since 30-bit versions of them must are
   used to represent Exclusive shadow states.  Are used as keys in
   WordFMs so must be castable to Words at the appropriate points. */
typedef  UInt  SegmentID;


/* These are handles for Word sets.  CONSTRAINTS: must be (very) small
   ints numbered from zero, since < 30-bit versions of them are used to
   encode thread-sets and lock-sets in 32-bit shadow words. */
typedef  WordSet  WordSetID;


/* Stores information about a thread.  Addresses of these also serve
   as unique thread identifiers and so are never freed, so they should
   be as small as possible. */
typedef
   struct _Thread {
      /* ADMIN */
      struct _Thread* admin;
      UInt            magic;
      /* USEFUL */
      WordSetID locksetA; /* WordSet of Lock* currently held by thread */
      WordSetID locksetW; /* subset of locksetA held in w-mode */
      SegmentID csegid;   /* current thread segment for thread */
      ThreadId  threadId; /* VG threadId / index of thread in map_threds */
      /* EXPOSITION */
      /* Place where parent was when this thread was created. */
      ExeContext* created_at;
      Bool        announced;
      Bool        ignore_reads; 
      /* Unique thread identifier for generating references in error messages
         and correctly calculating cmpGEQ_VTS even after thread exit. */
      Int         threadUID;
   }
   Thread;


/* Stores information about a lock's current state.  These are
   allocated and later freed (when the containing memory becomes
   NoAccess).  This gives a problem for the XError type, which
   contains Lock*s.  Solution is to copy any Lock which is to be
   incorporated into an XErrors, so as to make it independent from the
   'normal' collection of Locks, which can come and go.  When the lock
   is copied, its .magic is changed from LockN_Magic to
   LockP_Magic. */

/* Lock kinds. */
typedef
   enum {
      LK_mbRec=1001, /* normal mutex, possibly recursive */
      LK_nonRec,     /* normal mutex, definitely non recursive */
      LK_rdwr        /* reader-writer lock */
   }
   LockKind;

typedef
   struct _Lock {
      /* ADMIN */
      struct _Lock* admin;
      ULong         unique; /* used for persistence-hashing */
      UInt          magic;  /* LockN_MAGIC or LockP_MAGIC */
      /* EXPOSITION */
      /* Place where lock first came to the attention of Helgrind. */
      ExeContext*   appeared_at;
      /* If the lock is held, place where the lock most recently made
         an unlocked->locked transition.  Must be sync'd with .heldBy:
         if acquired_at is NULL then .heldBy must be empty, and vice
         versa. */
      ExeContext*   acquired_at;
      /* USEFUL-STATIC */
      Addr          guestaddr; /* Guest address of lock */
      LockKind      kind;      /* what kind of lock this is */
      /* USEFUL-DYNAMIC */
      Bool    heldW; 
      WordBag heldBy; /* bag of threads that hold this lock */
      /* HG_(isEmptyBag)(.heldBy) == True
         means:  lock is unheld, and heldW is meaningless
                 but arbitrarily set to False
         HG_(isEmptyBag)(.heldBy) == False
         means:
            lock is w-held by threads in .heldBy   if .heldW is True
            lock is r-held by threads in .heldBy   if .heldW is False

         for LK_nonRec, r-holdings are not allowed, and w-holdings may
         only have sizeTotal(heldBy) == 1

         for LK_mbRec, r-holdings are not allowed, and w-holdings may
         only have sizeUnique(heldBy) == 1

         for LK_rdwr, w-holdings may only have sizeTotal(.heldBy) == 1
      */
   }
   Lock;

/* Scalar Timestamp */
typedef
   struct {
      int  thrUID; /* threadUID of the thread with corresponding TS*/
      UInt tym;
   }
   ScalarTS;

/* Stores information about thread segments.  .prev can be NULL only
   when this is the first segment for the thread.  .other is NULL
   unless this segment depends on a message (create, join, signal)
   from some other thread.  Segments are never freed (!) */
typedef
   struct _Segment {
      DEBUG_ONLY(UInt magic);
      /* USEFUL */
      UInt             dfsver; /* Version # for depth-first searches */
      Thread*          thr;    /* The thread that I am part of */
      struct _Segment* prev;   /* The previous segment in this thread */
      struct _Segment* other;  /* Possibly a segment from some other 
                                  thread, which happened-before me */
      XArray*          vts;    /* XArray of ScalarTS */
      ExeContext       *context; 
      /* DEBUGGING ONLY: what does 'other' arise from?  
         c=thread creation, j=join, s=cvsignal, S=semaphore */
      DEBUG_ONLY(Char other_hint);
   }
   Segment;


/* This structure contains data from VG_USERREQ__HG_BENIGN_RACE or
   VG_USERREQ__HG_EXPECT_RACE client request.

   These two client requests are similar: they both suppress reports
   about a data race. The only difference is that for
   VG_USERREQ__HG_EXPECT_RACE helgrind will complain if the race was
   not detected (useful for unit tests). */
typedef 
   struct {
      Addr   addr;  ///< Data address (of race to ignore)
      HChar* descr; ///< Arbitrary text supplied by client. 
      HChar* file;  ///< File name (for debug output). 
      Int    line;  ///< Line number (for debug output)
      Bool detected; ///< Will be set once an error with 'ptr' is detected. 
      Bool is_benign; ///< True iff VG_USERREQ__HG_BENIGN_RACE was called. 
   }
   ExpectedError;


/* ------ CacheLine ------ */

#define N_LINE_BITS      6 /* must be >= 3 */
#define N_LINE_ARANGE    (1 << N_LINE_BITS)
#define N_LINE_TREES     (N_LINE_ARANGE >> 3)

typedef
   struct {
      UShort descrs[N_LINE_TREES];
      SVal   svals[N_LINE_ARANGE]; // == N_LINE_TREES * 8
   }
   CacheLine;

#define TREE_DESCR_16_0 (1<<0)
#define TREE_DESCR_32_0 (1<<1)
#define TREE_DESCR_16_1 (1<<2)
#define TREE_DESCR_64   (1<<3)
#define TREE_DESCR_16_2 (1<<4)
#define TREE_DESCR_32_1 (1<<5)
#define TREE_DESCR_16_3 (1<<6)
#define TREE_DESCR_8_0  (1<<7)
#define TREE_DESCR_8_1  (1<<8)
#define TREE_DESCR_8_2  (1<<9)
#define TREE_DESCR_8_3  (1<<10)
#define TREE_DESCR_8_4  (1<<11)
#define TREE_DESCR_8_5  (1<<12)
#define TREE_DESCR_8_6  (1<<13)
#define TREE_DESCR_8_7  (1<<14)
#define TREE_DESCR_DTY  (1<<15)

typedef
   struct {
      SVal  dict[4]; /* can represent up to 4 diff values in the line */
      UChar ix2s[N_LINE_ARANGE/4]; /* array of N_LINE_ARANGE 2-bit
                                      dict indexes */
      /* if dict[0] == 0 then dict[1] is the index of the CacheLineF
         to use */
   }
   CacheLineZ; /* compressed rep for a cache line */

typedef
   struct {
      Bool inUse;
      SVal w64s[N_LINE_ARANGE];
   }
   CacheLineF; /* full rep for a cache line */


/* Shadow memory.
   Primary map is a WordFM Addr SecMap*.  
   SecMaps cover some page-size-ish section of address space and hold
     a compressed representation.
   CacheLine-sized chunks of SecMaps are copied into a Cache, being
   decompressed when moved into the cache and recompressed on the
   way out.  Because of this, the cache must operate as a writeback
   cache, not a writethrough one.
*/
/* See comments below on shadow_mem_make_NoAccess re performance
   effects of N_SECMAP_BITS settings.  On a 2.4GHz Core2,
   starting/quitting OOo (32-bit), I have these rough numbers:
      N_SECMAP_BITS = 11    2m23
      N_SECMAP_BITS = 12    1m58
      N_SECMAP_BITS = 13    1m53

   Each SecMap must hold a power-of-2 number of CacheLines.  Hence
   N_SECMAP_BITS must >= N_LINE_BITS.
*/
#define N_SECMAP_BITS   13
#define N_SECMAP_ARANGE (1 << N_SECMAP_BITS)

// # CacheLines held by a SecMap
#define N_SECMAP_ZLINES (N_SECMAP_ARANGE / N_LINE_ARANGE)
typedef
   struct {
      UInt magic;
      Bool mbHasLocks;  /* hint: any locks in range?  safe: True */
      CacheLineZ  linesZ[N_SECMAP_ZLINES];
      CacheLineF* linesF;
      UInt linesF_size;
   }
   SecMap;

typedef
   struct {
      Int line_no; /* which Z-line are we in? */
      Int word_no; /* inside the line, which word is current? */
   }
   SecMapIter;

static void initSecMapIter ( SecMapIter* itr ) {
   itr->line_no = 0;
   itr->word_no = 0;
}

/* Get the current val, and move to the next position.  This is called
   a huge amount in some programs (eg OpenOffice).  Hence the
   'inline'. */
static UWord stats__secmap_iterator_steppings; /* fwds */

inline
static Bool stepSecMapIter ( /*OUT*/SVal** pVal, 
                             /*MOD*/SecMapIter* itr, SecMap* sm )
{
   CacheLineZ* lineZ = NULL;
   CacheLineF* lineF = NULL;
   /* Either it points to a valid place, or to (-1,-1) */
   stats__secmap_iterator_steppings++;
   if (UNLIKELY(itr->line_no == -1)) {
      tl_assert(itr->word_no == -1);
      return False;
   }
   /* so now it must be a valid place in the SecMap. */
   if (0) VG_(printf)("%p %d %d\n", sm, (Int)itr->line_no, (Int)itr->word_no);
   tl_assert(itr->line_no >= 0 && itr->line_no < N_SECMAP_ZLINES);
   lineZ = &sm->linesZ[itr->line_no];
   if (UNLIKELY(lineZ->dict[0] == 0)) {
      tl_assert(sm->linesF);
      tl_assert(sm->linesF_size > 0);
      tl_assert(lineZ->dict[1] >= 0);
      tl_assert(lineZ->dict[1] < sm->linesF_size);
      lineF = &sm->linesF[ lineZ->dict[1] ];
      tl_assert(lineF->inUse);
      tl_assert(itr->word_no >= 0 && itr->word_no < N_LINE_ARANGE);
      *pVal = &lineF->w64s[itr->word_no];
      itr->word_no++;
      if (itr->word_no == N_LINE_ARANGE)
         itr->word_no = 0;
   } else {
      tl_assert(itr->word_no >= 0 && itr->word_no <= 3);
      tl_assert(lineZ->dict[itr->word_no] != 0);
      *pVal = &lineZ->dict[itr->word_no];
      itr->word_no++;
      if (itr->word_no == 4 || lineZ->dict[itr->word_no] == 0)
         itr->word_no = 0;
   }

   if (itr->word_no == 0) {
      itr->line_no++;
      if (itr->line_no == N_SECMAP_ZLINES) {
         itr->line_no = -1;
         itr->word_no = -1;
      }
   }

   return True;
}

/* ------ Cache ------ */

#define N_WAY_BITS 16
#define N_WAY_NENT (1 << N_WAY_BITS)

/* Each tag is the address of the associated CacheLine, rounded down
   to a CacheLine address boundary.  A CacheLine size must be a power
   of 2 and must be 8 or more.  Hence an easy way to initialise the
   cache so it is empty is to set all the tag values to any value % 8
   != 0, eg 1.  This means all queries in the cache initially miss.
   It does however require us to detect and not writeback, any line
   with a bogus tag. */
typedef
   struct {
      CacheLine lyns0[N_WAY_NENT];
      Addr      tags0[N_WAY_NENT];
   }
   Cache;


/* --------- Primary data structures --------- */

/* Admin linked list of Threads */
static Thread* admin_threads = NULL;

/* Admin linked list of Locks */
static Lock* admin_locks = NULL;


/* Shadow memory primary map */
static WordFM* map_shmem = NULL; /* WordFM Addr SecMap* */
static Cache   cache_shmem;

/* Mapping table for core ThreadIds to Thread* */
static Thread** map_threads = NULL; /* Array[VG_N_THREADS] of Thread* */

/* Mapping table for lock guest addresses to Lock* */
static WordFM* map_locks = NULL; /* WordFM LockAddr Lock* */

/* The word-set universes for segment sets and lock sets. */
static WordSetU* univ_ssets = NULL; /* sets of SegmentsID */
static WordSetU* univ_lsets = NULL; /* sets of Lock* */
static WordSetU* univ_laog  = NULL; /* sets of Lock*, for LAOG */

/* never changed; we only care about its address.  Is treated as if it
   was a standard userspace lock.  Also we have a Lock* describing it
   so it can participate in lock sets in the usual way. */
static Int   __bus_lock = 0;
static Lock* __bus_lock_Lock = NULL;

static WordFM *map_expected_errors = NULL; /* WordFM Addr ExpectedError */



/*----------------------------------------------------------------*/
/*--- Simple helpers for the data structures                   ---*/
/*----------------------------------------------------------------*/

static UWord stats__lockN_acquires = 0;
static UWord stats__lockN_releases = 0;

static ThreadId map_threads_maybe_reverse_lookup ( Thread* ); /*fwds*/

#define Thread_MAGIC   0x504fc5e5
#define LockN_MAGIC    0x6545b557 /* normal nonpersistent locks */
#define LockP_MAGIC    0x755b5456 /* persistent (copied) locks */
#define Segment_MAGIC  0x49e94d81
#define SecMap_MAGIC   0x571e58cb

static UWord stats__mk_Segment = 0;
static UWord stats__fake_VTS_bytes_trashed = 0;

/* --------------- Segment vector --------------- */
// Segments are organized into N1 chunks, each chunk contains N2 Segments. 
// SegmentID is an index in this array of chunks: 
//   chunk_index    = SegmentID / N1
//   index_in_chunk = SegmentID % N1
// New chunks are allocated as needed.
//
// SegmentID=0 is not allowed (used for sanity checking).
// So, SegmentID > 0 && SegmentID < N1*N2. 
// 
// TODO: when we reach limit of N1*N2 segments we need to start 
// recycling old segments instead of exiting. 

#define SEGMENT_ID_MAX        (1 << 24) // N1*N2
#define SEGMENT_ID_CHUNK_SIZE (1 << 14) // N2
#define SEGMENT_ID_N_CHUNKS   (SEGMENT_ID_MAX / SEGMENT_ID_CHUNK_SIZE) // N1

static struct {
   UInt    size; 
   Segment *chunks[SEGMENT_ID_N_CHUNKS];
} SegmentArray ={1, {0}};


// Add a new segment, return its number
static SegmentID SEG_add(void) 
{
   UInt index, chunk_index, elem_index;
   // increment size 
   SegmentArray.size = (SegmentArray.size + 1) % SEGMENT_ID_MAX;
   if (SegmentArray.size == 0) {
      VG_(printf)("Helgrind: Fatal internal error -- cannot continue.\n");
      VG_(printf)("SegmentArray: wrapped around\n");
      tl_assert(0);
      SegmentArray.size = 2;
   }

   index = SegmentArray.size - 1;
   chunk_index = index / SEGMENT_ID_CHUNK_SIZE;
   elem_index  = index % SEGMENT_ID_CHUNK_SIZE;
   if (SegmentArray.chunks[chunk_index] == NULL) {
      // VG_(printf)("SegmentArray: new chunk %d\n", (int)chunk_index);
      SegmentArray.chunks[chunk_index]
         = hg_zalloc(sizeof(Segment) * SEGMENT_ID_CHUNK_SIZE);
   }
   VG_(memset)(&SegmentArray.chunks[chunk_index][elem_index], 
               0, sizeof(Segment));
   // VG_(printf)("SegmentArray: new segment %d\n", (int)index);
   return index;
}

static inline Bool SEG_id_is_sane(SegmentID n)
{
   return (n > 0) 
       && (n < SEGMENT_ID_MAX)
       && (SegmentArray.chunks[n / SEGMENT_ID_CHUNK_SIZE] != NULL);
}

static inline Segment *SEG_get(SegmentID n)
{  
   if (SCE_SVALS)
      tl_assert(SEG_id_is_sane(n));
   return &SegmentArray.chunks[n / SEGMENT_ID_CHUNK_SIZE]
                              [n % SEGMENT_ID_CHUNK_SIZE];
}

static inline Segment *SEG_maybe_get(SegmentID n)
{  
   if (!SEG_id_is_sane(n)) return NULL;
   return &SegmentArray.chunks[n / SEGMENT_ID_CHUNK_SIZE]
                              [n % SEGMENT_ID_CHUNK_SIZE];
}

static void SEG_set_context(SegmentID n, ExeContext *context) 
{
   if (SCE_SVALS)
      tl_assert(SEG_id_is_sane(n));
   SegmentArray.chunks[n / SEGMENT_ID_CHUNK_SIZE]
                      [n % SEGMENT_ID_CHUNK_SIZE].context = context;
}

static ExeContext *SEG_get_context(SegmentID n) 
{
   if (SCE_SVALS)
      tl_assert(SEG_id_is_sane(n));
   return SegmentArray.chunks[n / SEGMENT_ID_CHUNK_SIZE]
                             [n % SEGMENT_ID_CHUNK_SIZE].context;
}


/* --------- Constructors --------- */

static inline Bool is_sane_LockN ( Lock* lock ); /* fwds */

static Thread* mk_Thread ( SegmentID csegid, UWord threadId ) {
   static Int indx      = 1;
   Thread* thread       = hg_zalloc( sizeof(Thread) );
   thread->locksetA     = HG_(emptyWS)( univ_lsets );
   thread->locksetW     = HG_(emptyWS)( univ_lsets );
   thread->csegid       = csegid;
   thread->magic        = Thread_MAGIC;
   thread->created_at   = NULL;
   thread->announced    = False;
   thread->threadUID = indx++;
   thread->admin        = admin_threads;
   thread->threadId     = threadId;
   if (threadId != VG_INVALID_THREADID)
      map_threads[threadId] = thread;
   admin_threads        = thread;
   return thread;
}
// Make a new lock which is unlocked (hence ownerless)
static Lock* mk_LockN ( LockKind kind, Addr guestaddr ) {
   static ULong unique = 0;
   Lock* lock             = hg_zalloc( sizeof(Lock) );
   lock->admin            = admin_locks;
   lock->unique           = unique++;
   lock->magic            = LockN_MAGIC;
   lock->appeared_at      = NULL;
   lock->acquired_at      = NULL;
   lock->guestaddr        = guestaddr;
   lock->kind             = kind;
   lock->heldW            = False;
   HG_(initBag)(&lock->heldBy, hg_zalloc, hg_free);
   tl_assert(is_sane_LockN(lock));
   admin_locks            = lock;
   return lock;
}
static SegmentID mk_Segment ( Thread* thr, Segment* prev, Segment* other ) {
   SegmentID id    = SEG_add();
   Segment* seg    = SEG_get(id);
   seg->dfsver     = 0;
   seg->thr        = thr;
   seg->prev       = prev;
   seg->other      = other;
   seg->vts        = NULL;
   DEBUG_ONLY(seg->other_hint = ' ');
   DEBUG_ONLY(seg->magic = Segment_MAGIC);
   stats__mk_Segment++;
   return id;
}

static inline Bool is_sane_Segment ( Segment* seg ) {
   DEBUG_ONLY(return seg != NULL && seg->magic == Segment_MAGIC);
   RELEASE_ONLY(return 1);
}

static inline Bool is_sane_ThreadId ( ThreadId coretid ) {
   return coretid >= 0 && coretid < VG_N_THREADS;
}

static inline Bool is_sane_Thread ( Thread* thr ) {
   return thr != NULL && thr->magic == Thread_MAGIC
          && is_sane_ThreadId(thr->threadId)
          && (thr->threadId == VG_INVALID_THREADID
                || map_threads[thr->threadId] == thr);
}

static Bool is_sane_Bag_of_Threads ( WordBag* bag )
{
   Thread* thr;
   Word    count;
   HG_(initIterBag)( bag );
   while (HG_(nextIterBag)( bag, (Word*)&thr, &count )) {
      if (count < 1) return False;
      if (!is_sane_Thread(thr)) return False;
   }
   HG_(doneIterBag)( bag );
   return True;
}
static Bool is_sane_Lock_BASE ( Lock* lock )
{
   if (lock == NULL
       || (lock->magic != LockN_MAGIC && lock->magic != LockP_MAGIC))
      return False;
   switch (lock->kind) { 
      case LK_mbRec: case LK_nonRec: case LK_rdwr: break; 
      default: return False; 
   }
   if (HG_(isEmptyBag)(&lock->heldBy)) {
      if (lock->acquired_at != NULL) return False;
      /* Unheld.  We arbitrarily require heldW to be False. */
      return !lock->heldW;
   } else {
      if (lock->acquired_at == NULL) return False;
   }

   /* Lock is either r- or w-held. */
   if (!is_sane_Bag_of_Threads(&lock->heldBy)) 
      return False;
   if (lock->heldW) {
      /* Held in write-mode */
      if ((lock->kind == LK_nonRec || lock->kind == LK_rdwr)
          && !HG_(isSingletonTotalBag)(&lock->heldBy))
         return False;
   } else {
      /* Held in read-mode */
      if (lock->kind != LK_rdwr) return False;
   }
   return True;
}
static inline Bool is_sane_LockP ( Lock* lock ) {
   return lock != NULL 
          && lock->magic == LockP_MAGIC
          && is_sane_Lock_BASE(lock);
}
static inline Bool is_sane_LockN ( Lock* lock ) {
   return lock != NULL 
          && lock->magic == LockN_MAGIC
          && is_sane_Lock_BASE(lock);
}
static inline Bool is_sane_LockNorP ( Lock* lock ) {
   return is_sane_Lock_BASE(lock);
}

/* Release storage for a Lock.  Also release storage in .heldBy, if
   any. */
//static void del_LockN ( Lock* lk ) 
//{
//   tl_assert(is_sane_LockN(lk));
//   HG_(finiBag)( &lk->heldBy );
//   VG_(memset)(lk, 0xAA, sizeof(*lk));
//   hg_free(lk);
//}


/* Update 'lk' to reflect that 'thr' now has a write-acquisition of
   it.  This is done strictly: only combinations resulting from
   correct program and libpthread behaviour are allowed. */
static void lockN_acquire_writer ( Lock* lk, Thread* thr ) 
{
   tl_assert(is_sane_LockN(lk));
   tl_assert(is_sane_Thread(thr));

   stats__lockN_acquires++;

   /* EXPOSITION only */
   /* We need to keep recording snapshots of where the lock was
      acquired, so as to produce better lock-order error messages. */
   if (lk->acquired_at == NULL) {
      ThreadId tid;
      tl_assert(HG_(isEmptyBag)(&lk->heldBy));
      tid = map_threads_maybe_reverse_lookup(thr);
      lk->acquired_at = VG_(record_depth_N_ExeContext)
                           (tid, 0/*first_ip_delta*/, 
                                 clo_num_callers_for_locks);
   } else {
      tl_assert(!HG_(isEmptyBag)(&lk->heldBy));
   }
   /* end EXPOSITION only */

   switch (lk->kind) {
      case LK_nonRec:
      case_LK_nonRec:
         /* can't w-lock recursively */
         tl_assert(HG_(isEmptyBag)(&lk->heldBy));
         tl_assert(!lk->heldW);
         lk->heldW = True;
         HG_(addToBag)( &lk->heldBy, (Word)thr );
         break;
      case LK_mbRec:
         if (HG_(isEmptyBag)(&lk->heldBy))
            goto case_LK_nonRec;
         /* 2nd and subsequent locking of a lock by its owner */
         tl_assert(lk->heldW);
         /* assert: lk is only held by one thread .. */
         tl_assert(HG_(sizeUniqueBag(&lk->heldBy)) == 1);
         /* assert: .. and that thread is 'thr'. */
         tl_assert(HG_(elemBag)(&lk->heldBy, (Word)thr)
                   == HG_(sizeTotalBag)(&lk->heldBy));
         HG_(addToBag)(&lk->heldBy, (Word)thr);
         break;
      case LK_rdwr:
         /* must be unheld */
         tl_assert(HG_(isEmptyBag)(&lk->heldBy) && !lk->heldW);
         goto case_LK_nonRec;
      default: 
         tl_assert(0);
   }
   tl_assert(is_sane_LockN(lk));
}

static void lockN_acquire_reader ( Lock* lk, Thread* thr )
{
   tl_assert(is_sane_LockN(lk));
   tl_assert(is_sane_Thread(thr));
   /* can only add reader to a reader-writer lock. */
   tl_assert(lk->kind == LK_rdwr);
   /* lk must be free or already r-held. */
   tl_assert(HG_(isEmptyBag)(&lk->heldBy) 
             || (!HG_(isEmptyBag)(&lk->heldBy) && !lk->heldW));

   stats__lockN_acquires++;

   /* EXPOSITION only */
   /* We need to keep recording snapshots of where the lock was
      acquired, so as to produce better lock-order error messages. */
   if (lk->acquired_at == NULL) {
      ThreadId tid;
      tl_assert(HG_(isEmptyBag)(&lk->heldBy));
      tid = map_threads_maybe_reverse_lookup(thr);
      lk->acquired_at
         = VG_(record_depth_N_ExeContext)
              (tid, 0/*first_ip_delta*/, clo_num_callers_for_locks);
   } else {
      tl_assert(!HG_(isEmptyBag)(&lk->heldBy));
   }
   /* end EXPOSITION only */

   if (!HG_(isEmptyBag)(&lk->heldBy)) {
      HG_(addToBag)(&lk->heldBy, (Word)thr);
   } else {
      lk->heldW = False;
      HG_(addToBag)( &lk->heldBy, (Word)thr );
   }
   tl_assert(!lk->heldW);
   tl_assert(is_sane_LockN(lk));
}

/* Update 'lk' to reflect a release of it by 'thr'.  This is done
   strictly: only combinations resulting from correct program and
   libpthread behaviour are allowed. */

static void lockN_release ( Lock* lk, Thread* thr )
{
   Bool b;
   
   const int PP_MEM_INTERVAL = 10;
   
   // TODO: eliminate this hack (move somewhere else/optimize?) 
   static UInt lastCallTime = 0;
   UInt currTime;
   currTime = VG_(read_millisecond_timer)();
   if (UNLIKELY(currTime > lastCallTime + PP_MEM_INTERVAL * 1000)) {
      char temp[100] = "";
      VG_(sprintf) (temp, "%ds timer", PP_MEM_INTERVAL);
      pp_memory_usage (0, temp);
      pp_stats(temp);
      lastCallTime = currTime;
   }
   // <-- hack ends */
      
   tl_assert(is_sane_LockN(lk));
   tl_assert(is_sane_Thread(thr));
   /* lock must be held by someone */
   tl_assert(!HG_(isEmptyBag)(&lk->heldBy));
   stats__lockN_releases++;
   /* Remove it from the holder set */
   b = HG_(delFromBag)(&lk->heldBy, (Word)thr);
   /* thr must actually have been a holder of lk */
   tl_assert(b);
   /* normalise */
   tl_assert(lk->acquired_at);
   if (HG_(isEmptyBag)(&lk->heldBy)) {
      lk->heldW       = False;
      lk->acquired_at = NULL;
   }
   tl_assert(is_sane_LockN(lk));
}

static void remove_Lock_from_locksets_of_all_owning_Threads( Lock* lk )
{
   Thread* thr;
   if (HG_(isEmptyBag)(&lk->heldBy)) {
      tl_assert(!lk->heldW);
      return;
   }
   /* for each thread that holds this lock do ... */
   HG_(initIterBag)( &lk->heldBy );
   while (HG_(nextIterBag)( &lk->heldBy, (Word*)&thr, NULL )) {
      tl_assert(is_sane_Thread(thr));
      tl_assert(HG_(elemWS)( univ_lsets,
                             thr->locksetA, (Word)lk ));
      thr->locksetA
         = HG_(delFromWS)( univ_lsets, thr->locksetA, (Word)lk );

      if (lk->heldW) {
         tl_assert(HG_(elemWS)( univ_lsets,
                                thr->locksetW, (Word)lk ));
         thr->locksetW
            = HG_(delFromWS)( univ_lsets, thr->locksetW, (Word)lk );
      }
   }
   HG_(doneIterBag)( &lk->heldBy );
}

/* --------- Shadow memory --------- */

static inline Bool is_valid_scache_tag ( Addr tag ) {
   /* a valid tag should be naturally aligned to the start of
      a CacheLine. */
   return 0 == (tag & (N_LINE_ARANGE - 1));
}

static inline Bool is_sane_SecMap ( SecMap* sm ) {
   return sm != NULL && sm->magic == SecMap_MAGIC;
}


//
//   SVal:
//   10SSSSSSSSSSSSSSSSSSSSSSSSSSrrrrTrrrrrrrLLLLLLLLLLLLLLLLLLLLLLLL Read
//   11SSSSSSSSSSSSSSSSSSSSSSSSSSrrrrTrrrrrrrLLLLLLLLLLLLLLLLLLLLLLLL Mod
//     \_______ 26 _____________/            \________ 24 __________/
// 
//   0100000000000000000000000000000000000000000000000000000000000001 Ignore
//   0000000000000000000000000000000000000000000000000000001000000000 New
//   0000000000000000000000000000000000000000000000000000000000000000 Invalid
//   \______________________________64______________________________/
//
//   r - reserved bits 
//   S - segment set bits 
//   L - lock set bits 
//   T - trace bit
//
//   It's crucial that no valid SVal has a value of zero, since zero
//   has a special meaning for the LineZ/LineF mechanism (see
//   "lineZ->dict[0] == 0" in get_ZF_by_index).
//
//
//   S: 
//   1SSSSSSSSSSSSSSSSSSSSSSS Just one segment. 
//   0SSSSSSSSSSSSSSSSSSSSSSS A real segment set. 
//
//   T (trace bit):
//   When set, the trace bit indicates that we want to trace each access 
//   to this memory (we output the trace only when the state changes).
//
//
//   

//------------- segment set, lock set --------------

#define N_SEG_SET_BITS     26
#define N_LOCK_SET_BITS    24
#define TRACE_BIT_POSITION 31

#define SHVAL_New       ((SVal)(2<<8))
#define SHVAL_Invalid   ((SVal)(0))
#define SHVAL_Ignore    ((SVal)((1ULL << 62)+1))

typedef  UInt       SegmentSet;
typedef  WordSetID  LockSet;  /* UInt */

static inline Bool SS_valid (SegmentSet ss) {
   return ss < (1 << N_SEG_SET_BITS);
}

static inline Bool SS_is_singleton (SegmentSet ss) {
   return (ss & (1 << (N_SEG_SET_BITS-1))) != 0;
}

static inline UWord SS_get_size (SegmentSet ss) {
   if (SS_is_singleton(ss)) return 1;
   tl_assert(HG_(cardinalityWSU)(univ_ssets) > ss);
   return HG_(cardinalityWS)(univ_ssets, ss);
}

static inline SegmentSet SS_mk_singleton (SegmentID ss) {
   if (SCE_SVALS)
      tl_assert(SEG_id_is_sane(ss));
   ss |= (1 << (N_SEG_SET_BITS-1));
   if (SCE_SVALS)
      tl_assert(SS_is_singleton(ss));
   return ss;
}

static inline SegmentID SS_get_singleton (SegmentSet ss) {
   tl_assert(SS_is_singleton(ss));
   ss &= ~(1 << (N_SEG_SET_BITS-1));
   tl_assert(SEG_id_is_sane(ss));
   return ss;
}

static inline SegmentID SS_get_singleton_UNCHECKED (SegmentSet ss) {
   ss &= ~(1 << (N_SEG_SET_BITS-1));
   if (SCE_SVALS)
      tl_assert(SEG_id_is_sane(ss));
   return ss;
}

static inline SegmentID SS_get_element (SegmentSet ss, UWord i) {
   UWord nWords, *words;
   if (SS_is_singleton(ss))
      return SS_get_singleton(ss);
   HG_(getPayloadWS)( &words, &nWords, univ_ssets, ss );
   tl_assert(i < nWords);
   return words[i];
}

// increment the ref count for a non-singleton SS. 
static inline void SS_ref(SegmentSet ss, UWord sz) {
   tl_assert(!SS_is_singleton(ss));
   HG_(refWS) (univ_ssets, ss, sz);
}


// decrement the ref count of a non-singleton SS and return 
// the new value of ref count
static inline UInt SS_unref(SegmentSet ss, UWord sz) {
   tl_assert(!SS_is_singleton(ss));
   return HG_(unrefWS) (univ_ssets, ss, sz);
}

static inline Bool LS_valid (LockSet ls) {
   return ls < (1 << N_LOCK_SET_BITS);
}

static inline SVal mk_SHVAL_RM (Bool is_m, SegmentSet ss, LockSet ls) {
   SVal res;
   if (SCE_SVALS) {
      tl_assert(SS_valid(ss));
      tl_assert(LS_valid(ls));
   }
   res = (1ULL << 63) 
         |  ((SVal)is_m << 62) 
         |  ((SVal)ss << (62-N_SEG_SET_BITS)) 
         |  ((SVal)ls);
   //  VG_(printf)("XX %llx\n", res);
   return res;
}
static inline SVal mk_SHVAL_R (SegmentSet ss, LockSet ls) {
   return mk_SHVAL_RM(False, ss, ls);
}
static inline SVal mk_SHVAL_M (SegmentSet ss, LockSet ls) {
   return mk_SHVAL_RM(True, ss, ls);
}

static inline Bool get_SHVAL_TRACE_BIT (SVal sv) {
   return 1 == ((sv >> TRACE_BIT_POSITION) & 1);
}

static inline SVal set_SHVAL_TRACE_BIT (SVal sv, Bool trace_bit) {
   return sv | ((SVal)trace_bit << TRACE_BIT_POSITION);
}


static inline SegmentSet get_SHVAL_SS (SVal sv) {
   SegmentSet ss;
   Int   shift = 62 - N_SEG_SET_BITS;
   ULong mask  = (1 << N_SEG_SET_BITS) - 1;
   ss = (sv >> shift) & mask;
   tl_assert(SS_valid(ss));
   return ss;
}
static inline LockSet get_SHVAL_LS (SVal sv) {
   LockSet ls;
   ls = sv & ((1ULL << N_LOCK_SET_BITS) - 1);
   tl_assert(LS_valid(ls));
   return ls;
}

static inline Bool is_SHVAL_RM (SVal sv) {
   /* Top 2 bits are 10 (R) or 11 (M) */
   return (sv >> 63) != 0;
} 
static inline Bool is_SHVAL_R (SVal sv) {
   return ((sv >> 62) & 3) == 2; /* == 10 (R) */
}
static inline Bool is_SHVAL_M (SVal sv) {
   return ((sv >> 62) & 3) == 3; /* == 11 (W) */
}

static inline Bool is_SHVAL_Shared (SVal sv) {
   return is_SHVAL_RM(sv) && !SS_is_singleton(get_SHVAL_SS(sv));
}

static inline Bool is_SHVAL_New    (SVal sv) {return sv == SHVAL_New;}
static inline Bool is_SHVAL_Ignore (SVal sv) {return sv == SHVAL_Ignore;}

static inline Bool is_SHVAL_valid ( SVal sv) {
   return is_SHVAL_RM(sv) || is_SHVAL_New(sv)
          || is_SHVAL_Ignore(sv)
          ;
}


static inline Bool is_SHVAL_valid_SLOW ( SVal sv) {
   if (!is_SHVAL_valid(sv)) return False;
#if 0
   if (clo_ss_recycle && is_SHVAL_RM(sv)) {
      SegmentSet SS = get_SHVAL_SS(sv); 
      if (!SS_is_singleton(SS)) {
         if (!(HG_(saneWS_SLOW(univ_ssets, SS)))) {
            VG_(printf)("not sane: %llx %llx\n", sv, (sv >> 26) & 15);
            return False;
         }
      }
   }
#endif   
   return True;
}

// If sv has a non-singleton SS, increment it's refcount by 1.
static inline void SHVAL_SS_ref(SVal sv) {
   if (LIKELY(is_SHVAL_RM(sv))) {
      SegmentSet ss = get_SHVAL_SS(sv);
      if (UNLIKELY(!SS_is_singleton(ss))) {
         SS_ref(ss, 1);
      }
   }
}

// If sv has a non-singleton SS, decrement it's refcount by 1.
static inline void SHVAL_SS_unref(SVal sv) {
   if (LIKELY(is_SHVAL_RM(sv))) {
      SegmentSet ss = get_SHVAL_SS(sv);
      if (UNLIKELY(!SS_is_singleton(ss))) {
         SS_unref(ss, 1);
      }
   }
}

/*----------------------------------------------------------------*/
/*--- Print out the primary data structures                    ---*/
/*----------------------------------------------------------------*/

// static WordSetID del_BHL ( WordSetID lockset ); /* fwds */
static 
void get_ZF_by_index ( /*OUT*/CacheLineZ** zp, /*OUT*/CacheLineF** fp,
                       SecMap* sm, UInt zix ); /* fwds */

#define PP_THREADS      (1<<1)
#define PP_LOCKS        (1<<2)
#define PP_SEGMENTS     (1<<3)
#define PP_SHMEM_SHARED (1<<4)
#define PP_ALL (PP_THREADS | PP_LOCKS | PP_SEGMENTS | PP_SHMEM_SHARED)


static const Int sHOW_ADMIN = 0;

static void space ( Int n )
{
   Int  i;
   Char spaces[128+1];
   tl_assert(n >= 0 && n < 128);
   if (n == 0)
      return;
   for (i = 0; i < n; i++)
      spaces[i] = ' ';
   spaces[i] = 0;
   tl_assert(i < 128+1);
   VG_(printf)("%s", spaces);
}

static void pp_Thread ( Int d, Thread* t )
{
   space(d+0); VG_(printf)("Thread %p {\n", t);
   if (sHOW_ADMIN) {
   space(d+3); VG_(printf)("admin    %p\n",   t->admin);
   space(d+3); VG_(printf)("magic    0x%x\n", (UInt)t->magic);
   }
   space(d+3); VG_(printf)("locksetA %d\n",   (Int)t->locksetA);
   space(d+3); VG_(printf)("locksetW %d\n",   (Int)t->locksetW);
   space(d+3); VG_(printf)("csegid   0x%x\n", (UInt)t->csegid);
   space(d+0); VG_(printf)("}\n");
}

static void pp_admin_threads ( Int d )
{
   Int     i, n;
   Thread* t;
   for (n = 0, t = admin_threads;  t;  n++, t = t->admin) {
      /* nothing */
   }
   space(d); VG_(printf)("admin_threads (%d records) {\n", n);
   for (i = 0, t = admin_threads;  t;  i++, t = t->admin) {
      if (0) {
         space(d+3); 
         VG_(printf)("admin_threads record %d of %d:\n", i, n);
      }
      pp_Thread(d+3, t);
   }
   space(d); VG_(printf)("}\n", n);
}

static void pp_mem_admin_threads ( Int d )
{
   Int     i, n;
   Word admin_threads_bytes = 0;
   Thread* t;
   for (n = 0, i = 0, t = admin_threads;  t;  n++, i++, t = t->admin) {
      admin_threads_bytes += sizeof(Thread);
      // TODO: csegid ?
   }
   space(d); VG_(printf)("threads:  %7d kB (count = %d)\n",
         (int)(admin_threads_bytes/1024), n);
}

static void pp_map_threads ( Int d )
{
   Int i, n;
   n = 0;
   space(d); VG_(printf)("map_threads ");
   n = 0;
   for (i = 0; i < VG_N_THREADS; i++) {
      if (map_threads[i] != NULL)
         n++;
   }
   VG_(printf)("(%d entries) {\n", n);
   for (i = 0; i < VG_N_THREADS; i++) {
      if (map_threads[i] == NULL)
         continue;
      space(d+3); VG_(printf)("coretid %d -> Thread %p\n", i, map_threads[i]);
   }
   space(d); VG_(printf)("}\n");
}

static const HChar* show_LockKind ( LockKind lkk ) {
   switch (lkk) {
      case LK_mbRec:  return "mbRec";
      case LK_nonRec: return "nonRec";
      case LK_rdwr:   return "rdwr";
      default:        tl_assert(0);
   }
}

static void pp_Lock ( Int d, Lock* lk )
{
   space(d+0); VG_(printf)("Lock %p (ga %p) {\n", lk, lk->guestaddr);
   if (sHOW_ADMIN) {
      space(d+3); VG_(printf)("admin  %p\n",   lk->admin);
      space(d+3); VG_(printf)("magic  0x%x\n", (UInt)lk->magic);
   }
   space(d+3); VG_(printf)("unique %llu\n", lk->unique);
   space(d+3); VG_(printf)("kind   %s\n", show_LockKind(lk->kind));
   space(d+3); VG_(printf)("heldW  %s", lk->heldW ? "yes" : "no");
   //space(d+3); VG_(printf)("\nheldBy %p", lk->heldBy);
   if (!HG_(isEmptyBag)(&lk->heldBy)) {
      Thread* thr;
      Word    count;
      VG_(printf)(" { ");
      HG_(initIterBag)( &lk->heldBy );
      while (HG_(nextIterBag)( &lk->heldBy, (Word*)&thr, &count ))
         VG_(printf)("%lu:%p ", count, thr);
      HG_(doneIterBag)( &lk->heldBy );
      VG_(printf)("}");
   }
   VG_(printf)("\n");
   space(d+0); VG_(printf)("}\n");
}

static void pp_admin_locks ( Int d )
{
   Int   i, n;
   Lock* lk;
   for (n = 0, lk = admin_locks;  lk;  n++, lk = lk->admin) {
      /* nothing */
   }
   space(d); VG_(printf)("admin_locks (%d records) {\n", n);
   for (i = 0, lk = admin_locks;  lk;  i++, lk = lk->admin) {
      if (0) {
         space(n); VG_(printf)("admin_locks record %d of %d:\n", i, n);
      }
      pp_Lock(d+3, lk);
   }
   space(d); VG_(printf)("}\n", n);
}

static void pp_mem_admin_locks ( Int d )
{
   UWord i, n = -1, lockset_bytes;
   Lock* lk;
   Word admin_locks_bytes = 0;
   for (n = 0, i = 0, lk = admin_locks;  lk;  n++, i++, lk = lk->admin) {
      admin_locks_bytes += sizeof(Lock);
      // TODO: lk->heldBy ?
   }
   space(d); VG_(printf)("locks:    %7d kB (count = %d)\n",
         (int)(admin_locks_bytes/1024), n);
   
   // LockSets below   
   lockset_bytes = HG_(memoryConsumedWSU) (univ_lsets, &n);
   space(d); VG_(printf)("locksets: %7d kB (active = %d, total = %d)\n",
         (int)(lockset_bytes/1024), n, HG_(cardinalityWSU) (univ_lsets));
}

static void pp_map_locks ( Int d )
{
   void* gla;
   Lock* lk;
   space(d); VG_(printf)("map_locks (%d entries) {\n",
                         (Int)HG_(sizeFM)( map_locks ));
   HG_(initIterFM)( map_locks );
   while (HG_(nextIterFM)( map_locks, (Word*)&gla,
                                      (Word*)&lk )) {
      space(d+3);
      VG_(printf)("guest %p -> Lock %p\n", gla, lk);
   }
   HG_(doneIterFM)( map_locks );
   space(d); VG_(printf)("}\n");
}

static void pp_Segment ( Int d, Segment* s )
{
   space(d+0); VG_(printf)("Segment %p {\n", s);
   if (sHOW_ADMIN) {
   DEBUG_ONLY(space(d+3); VG_(printf)("magic  0x%x\n", (UInt)s->magic));
   }
   space(d+3); VG_(printf)("dfsver    %u\n", s->dfsver);
   space(d+3); VG_(printf)("thr       %p\n", s->thr);
   space(d+3); VG_(printf)("prev      %p\n", s->prev);
   DEBUG_ONLY(space(d+3); VG_(printf)("other[%c] %p\n", 
                                 s->other_hint, s->other));
   space(d+0); VG_(printf)("}\n");
}

static void pp_all_segments ( Int d )
{
   ULong i;
   space(d); VG_(printf)("map_segments (%d entries) {\n", 
                         (Int)SegmentArray.size);
   for (i = 1; i < SegmentArray.size; i++) {
      // space(d+3); VG_(printf)("segid 0x%x -> Segment %p\n", (UInt)i, SEG_get(i));
      pp_Segment(d+3, SEG_get(i));
   }
   space(d); VG_(printf)("}\n");
}

static void pp_mem_segments ( Int d )
{
   ULong i;
   UWord n = -1, segments_bytes = 0, ss_bytes = 0;
   for (i = 1; i < SegmentArray.size; i++) {
      Segment* s = SEG_get(i);
      segments_bytes += sizeof(Segment);
      if (s->vts)
         segments_bytes += VG_(bytesXA)(s->vts);
      // TODO: sizeof(ExeContext) ???
   }
   space(d); VG_(printf)("segments: %7d kB (count = %d) \n",
         (int)(segments_bytes / 1024), (Int)SegmentArray.size);
   space(d); VG_(printf)("Trashed fake-VTS bytes: %7d kB\n",
                  (Int)(stats__fake_VTS_bytes_trashed/1024));
   pp_fake_segment_stats(d);

   // SegmentSets below
   ss_bytes = HG_(memoryConsumedWSU) (univ_ssets, &n);
   space(d); VG_(printf)("seg.sets: %7d kB (active = %d, total = %d)\n",
         (int)(ss_bytes/1024), n, HG_(cardinalityWSU) (univ_ssets));
}

// Just print all locks in the lockset 'ls'. 
static void show_lockset(LockSet ls)
{
   UWord* word;
   UWord  nWords, i;
   HG_(getPayloadWS)( &word, &nWords, univ_lsets, ls );
   if(nWords == 0) {
      VG_(message)(Vg_UserMsg, "   None");
   }

   for (i = 0; i < nWords; i++) {
      Lock* lk = (Lock*)word[i];
      //      VG_(message)(Vg_UserMsg, "   L:%p/%p", lk, lk->guestaddr);
      VG_(message)(Vg_UserMsg, "   Lock located at %p and first observed",
                               lk->guestaddr);
      if (lk->acquired_at) {
         VG_(pp_ExeContext)(lk->acquired_at);
      }  
   }
}

// KCC: do we need two separate print functions (for user and not for user)?
// Print the SVal in human readable form.
static void show_sval ( /*OUT*/Char* buf, Int nBuf, SVal sv )
{
   tl_assert(nBuf-1 >= 99);
   VG_(memset)(buf, 0, nBuf);
   if (is_SHVAL_New(sv)) {
      VG_(sprintf)(buf, "%s", "New");
   } else if (is_SHVAL_Ignore(sv)) {
      VG_(sprintf)(buf, "%s", "Ignore");
   } else if (is_SHVAL_RM(sv)) {
      UWord i;
      Bool is_m     = is_SHVAL_M(sv);
      SegmentSet ss = get_SHVAL_SS(sv);
      LockSet    ls = get_SHVAL_LS(sv);
      UWord n_segments = SS_get_size(ss);
      Int n_locks    = HG_(cardinalityWS)(univ_lsets, ls);
#if 0
      HChar* name    = is_m ? (n_segments > 1 ? "ShM" : "ExM")
                            : (n_segments > 1 ? "ShR" : "ExR" );
#else
      HChar* name    = is_m ? "Mod" : "RdO";
#endif
      VG_(sprintf)(buf, "%s; #LS=%d; #SS=%d; ", 
                   name, n_locks, (Int)n_segments);

      for (i = 0; i < n_segments; i++) {
         SegmentID S;
         if (VG_(strlen(buf)) > nBuf - 20) {
            VG_(sprintf)(buf + VG_(strlen)(buf), "...");
            break;
         }
         S = SS_get_element(ss, i);
         if (i > 0)  VG_(sprintf)(buf + VG_(strlen)(buf), ", ");
         VG_(sprintf)(buf + VG_(strlen)(buf), "T%d/S%d", 
                      (Int)SEG_get(S)->thr->threadUID, (Int)S);
      }
   } else {
      VG_(sprintf)(buf, "Invalid-shadow-word(%u)", sv);
   }
}

static void pp_SecMap_shared ( Int d, SecMap* sm, Addr ga )
{
   Int  i;
#if 0
   Addr a;
   SVal w32;
   Char buf[100];
#endif
   CacheLineZ* lineZ;
   CacheLineF* lineF;
   space(d+0); VG_(printf)("SecMap %p (ga %p) {\n", sm, (void*)ga);

   for (i = 0; i < N_SECMAP_ZLINES; i++) {
      get_ZF_by_index( &lineZ, &lineF, sm, i );
      space(d+3); VG_(printf)("// pp_SecMap_shared: not implemented\n");
   }

#if 0
   for (i = 0; i < N_SECMAP_ARANGE; i++) {
      w32 = sm->w32s[i];
      a   = ga + 1 * i;
      if (! (is_SHVAL_ShM(w32) || is_SHVAL_ShR(w32)))
         continue;
      space(d+3); VG_(printf)("%p -> 0x%08x ", (void*)a, w32);
      show_sval(buf, sizeof(buf), w32);
      VG_(printf)("%s\n", buf);
   }
#endif

   space(d+0); VG_(printf)("}\n");
}

static void pp_map_shmem_shared ( Int d )
{
   Addr    ga;
   SecMap* sm;
   space(d); VG_(printf)("map_shmem_ShR_and_ShM_only {\n");
   HG_(initIterFM)( map_shmem );
   while (HG_(nextIterFM)( map_shmem, (Word*)&ga,
                                      (Word*)&sm )) {
      pp_SecMap_shared( d+3, sm, ga );
   }
   HG_(doneIterFM) ( map_shmem );
   space(d); VG_(printf)("}\n");
}

static void pp_mem_shmem ( Int d ) {
   Word shmem_size_bytes = 0,
        compressed_bytes = 0;
   Addr    ga;
   SecMap* sm;
   HG_(initIterFM)( map_shmem );
   while (HG_(nextIterFM)( map_shmem, (Word*)&ga,
                                      (Word*)&sm )) {
      /*
       * Was:
       *    shmem_size_bytes += N_SECMAP_ARANGE;
       * which means completely wrong :-) 
       */
      shmem_size_bytes += N_SECMAP_ARANGE * sizeof(SVal);
      compressed_bytes += sizeof(Word) 
            + N_SECMAP_ZLINES * sizeof(CacheLineZ)
            + sm->linesF_size * sizeof(CacheLineF);
   }
   HG_(doneIterFM) ( map_shmem );
   space(d); VG_(printf)("sh.memory %7d kB (uncompressed = %d kB)\n", 
         (int)(compressed_bytes/1024), (int)(shmem_size_bytes/1024)); 
}

static void pp_everything ( Int flags, Char* caller )
{
   Int d = 0;
   VG_(printf)("\n");
   VG_(printf)("All_Data_Structures (caller = \"%s\") {\n", caller);
   if (flags & PP_THREADS) {
      VG_(printf)("\n");
      pp_admin_threads(d+3);
      VG_(printf)("\n");
      pp_map_threads(d+3);
   }
   if (flags & PP_LOCKS) {
      VG_(printf)("\n");
      pp_admin_locks(d+3);
      VG_(printf)("\n");
      pp_map_locks(d+3);
   }
   if (flags & PP_SEGMENTS) {
      VG_(printf)("\n");
      pp_all_segments(d+3);
   }
   if (flags & PP_SHMEM_SHARED) {
      VG_(printf)("\n");
      pp_map_shmem_shared( d+3 );
   }

   VG_(printf)("\n");
   VG_(printf)("}\n");
   VG_(printf)("\n");
}

static void pp_memory_usage ( Int flags, Char* caller )
{  
   Int d = 0;
   if (flags == 0)
      flags = PP_ALL;   
   VG_(printf)("\n");
   VG_(printf)("Memory usage statistics (caller = \"%s\") {\n"
               "   time: %9dms from start\n", 
            caller, (int)VG_(read_millisecond_timer)());
   if (flags & PP_THREADS) {
      pp_mem_admin_threads(d+3);
   }
   if (flags & PP_LOCKS) {
      pp_mem_admin_locks(d+3);
   }
   if (flags & PP_SEGMENTS) {
      pp_mem_segments(d+3);
   }
   pp_mem_laog (d+3);
   if (flags & PP_SHMEM_SHARED) {
      pp_mem_shmem(d+3);
   }
   VG_(printf)("}\n");
   VG_(printf)("\n");
}

#undef SHOW_ADMIN


/*----------------------------------------------------------------*/
/*--- Initialise the primary data structures                   ---*/
/*----------------------------------------------------------------*/

/* fwds */
static void shmem__invalidate_scache ( void );
static void hbefore__invalidate_htable ( void );
static void shmem__set_mbHasLocks ( Addr a, Bool b );
static Bool shmem__get_mbHasLocks ( Addr a );
static void shadow_mem_set8 ( Thread* uu_thr_acc, Addr a, SVal svNew );
static XArray* singleton_VTS ( Thread* thr, UInt tym );

static void initialise_data_structures ( void )
{
   SegmentID segid;
   Segment*  seg;
   Thread*   thr;

   /* Get everything initialised and zeroed. */
   tl_assert(admin_threads == NULL);
   tl_assert(admin_locks == NULL);

   tl_assert(sizeof(Addr) == sizeof(Word));
   tl_assert(map_shmem == NULL);
   map_shmem = HG_(newFM)( hg_zalloc, hg_free, NULL/*unboxed Word cmp*/);
   tl_assert(map_shmem != NULL);
   shmem__invalidate_scache();

   tl_assert(map_threads == NULL);
   map_threads = hg_zalloc( VG_N_THREADS * sizeof(Thread*) );
   tl_assert(map_threads != NULL);

   /* re <=: < on 64-bit platforms, == on 32-bit ones */
   tl_assert(sizeof(SegmentID) <= sizeof(Word));
   tl_assert(sizeof(Segment*) == sizeof(Word));
   hbefore__invalidate_htable();

   tl_assert(sizeof(Addr) == sizeof(Word));
   tl_assert(map_locks == NULL);
   map_locks = HG_(newFM)( hg_zalloc, hg_free, NULL/*unboxed Word cmp*/);
   tl_assert(map_locks != NULL);

   __bus_lock_Lock = mk_LockN( LK_nonRec, (Addr)&__bus_lock );
   tl_assert(is_sane_LockN(__bus_lock_Lock));
   HG_(addToFM)( map_locks, (Word)&__bus_lock, (Word)__bus_lock_Lock );

   tl_assert(univ_ssets == NULL);
   univ_ssets = HG_(newWordSetU)( hg_zalloc, hg_free, 64/*cacheSize*/ );
   tl_assert(univ_ssets != NULL);

   tl_assert(univ_lsets == NULL);
   univ_lsets = HG_(newWordSetU)( hg_zalloc, hg_free, 12/*cacheSize*/ );
   tl_assert(univ_lsets != NULL);

   tl_assert(univ_laog == NULL);
   univ_laog = HG_(newWordSetU)( hg_zalloc, hg_free, 32/*cacheSize*/ );
   tl_assert(univ_laog != NULL);

   tl_assert(map_expected_errors == NULL);
   map_expected_errors = HG_(newFM)( hg_zalloc, hg_free, NULL /*unboxed cmp*/);
   tl_assert(map_expected_errors != NULL);

   /* Set up entries for the root thread */
   // FIXME: this assumes that the first real ThreadId is 1

   /* a segment for the new thread ... */
   // FIXME: code duplication in ev__post_thread_create
   segid   = mk_Segment( NULL, NULL, NULL );
   seg     = SEG_get(segid);

   /* a Thread for the new thread and bind it in the thread-map table.
      FIXME: assumes root ThreadId == 1. */
   thr = mk_Thread( segid, 1 );
   seg->thr = thr;

   /* Give the thread a starting-off vector timestamp. */
   seg->vts = singleton_VTS( seg->thr, 1 );

   tl_assert(VG_INVALID_THREADID == 0);

   /* Mark the new bus lock correctly (to stop the sanity checks
      complaining) */
   tl_assert( sizeof(__bus_lock) == 4 );
   shadow_mem_set8( NULL/*unused*/, __bus_lock_Lock->guestaddr, 
                                    SHVAL_New );
   shmem__set_mbHasLocks( __bus_lock_Lock->guestaddr, True );

   all__sanity_check("initialise_data_structures");
}


/*----------------------------------------------------------------*/
/*--- map_threads :: WordFM core-ThreadId Thread*              ---*/
/*----------------------------------------------------------------*/

/* Doesn't assert if the relevant map_threads entry is NULL. */
static Thread* map_threads_maybe_lookup ( ThreadId coretid )
{
   Thread* thr;
   tl_assert( is_sane_ThreadId(coretid) );
   thr = map_threads[coretid];
   return thr;
}

/* Asserts if the relevant map_threads entry is NULL. */
static inline Thread* map_threads_lookup ( ThreadId coretid )
{
   Thread* thr;
   tl_assert( is_sane_ThreadId(coretid) );
   thr = map_threads[coretid];
   tl_assert(thr);
   return thr;
}

static inline Thread* map_threads_lookup_by_UID_SLOW (int threadUID) {
   UInt i;
   for (i = 0; i < VG_N_THREADS; i++)
      if (map_threads[i] && map_threads[i]->threadUID == threadUID)
         return map_threads[i];
   return NULL;
}

/* Do a reverse lookup.  Warning: POTENTIALLY SLOW.  Does not assert
   if 'thr' is not found in map_threads. */
static ThreadId map_threads_maybe_reverse_lookup ( Thread* thr )
{
   tl_assert(is_sane_Thread(thr));
   /* Check nobody used the invalid-threadid slot */
   tl_assert(VG_INVALID_THREADID >= 0 && VG_INVALID_THREADID < VG_N_THREADS);
   tl_assert(map_threads[VG_INVALID_THREADID] == NULL);
   // hmmm, but the fake_thread in evhH__do_cv_signal does... 
   return thr->threadId;
}

/* Do a reverse lookup.  Warning: POTENTIALLY SLOW.  Asserts if 'thr'
   is not found in map_threads. */
static ThreadId map_threads_reverse_lookup ( Thread* thr )
{
   ThreadId tid = map_threads_maybe_reverse_lookup( thr );
   tl_assert(tid != VG_INVALID_THREADID);
   return tid;
}

static void map_threads_delete ( ThreadId coretid )
{
   /* From now on, you can't use threadId to search thread
      because it may be reused by valgrind.
      If you still need to identify thread with something except Thread*, use
      threadUID instead. */
   Thread* thr;
   tl_assert(coretid != 0);
   tl_assert( is_sane_ThreadId(coretid) );
   thr = map_threads[coretid];
   tl_assert(thr);
   if (coretid != VG_INVALID_THREADID)
      map_threads[coretid] = NULL;
   thr->threadId = VG_INVALID_THREADID;
}


/*----------------------------------------------------------------*/
/*--- map_locks :: WordFM guest-Addr-of-lock Lock*             ---*/
/*----------------------------------------------------------------*/

/* Make sure there is a lock table entry for the given (lock) guest
   address.  If not, create one of the stated 'kind' in unheld state.
   In any case, return the address of the existing or new Lock. */
static 
Lock* map_locks_lookup_or_create ( LockKind lkk, Addr ga, ThreadId tid )
{
   Bool  found;
   Lock* oldlock = NULL;
   tl_assert(is_sane_ThreadId(tid));
   found = HG_(lookupFM)( map_locks, 
                          NULL, (Word*)&oldlock, (Word)ga );
   if (!found) {
      Lock* lock = mk_LockN(lkk, ga);
      lock->appeared_at = VG_(record_ExeContext)( tid, 0 );
      tl_assert(is_sane_LockN(lock));
      HG_(addToFM)( map_locks, (Word)ga, (Word)lock );
      tl_assert(oldlock == NULL);
      // mark the relevant secondary map has .mbHasLocks
      shmem__set_mbHasLocks( ga, True );
      return lock;
   } else {
      tl_assert(oldlock != NULL);
      tl_assert(is_sane_LockN(oldlock));
      tl_assert(oldlock->guestaddr == ga);
      // check the relevant secondary map has .mbHasLocks?
      tl_assert(shmem__get_mbHasLocks(ga) == True);
      return oldlock;
   }
}

static Lock* map_locks_maybe_lookup ( Addr ga )
{
   Bool  found;
   Lock* lk = NULL;
   found = HG_(lookupFM)( map_locks, NULL, (Word*)&lk, (Word)ga );
   tl_assert(found  ?  lk != NULL  :  lk == NULL);
   if (found) {
      // check the relevant secondary map has .mbHasLocks?
      tl_assert(shmem__get_mbHasLocks(ga) == True);
   }
   return lk;
}

static void map_locks_delete ( Addr ga )
{
   Addr  ga2 = 0;
   Lock* lk  = NULL;
   HG_(delFromFM)( map_locks,
                   (Word*)&ga2, (Word*)&lk, (Word)ga );
   /* delFromFM produces the val which is being deleted, if it is
      found.  So assert it is non-null; that in effect asserts that we
      are deleting a (ga, Lock) pair which actually exists. */
   tl_assert(lk != NULL);
   tl_assert(ga2 == ga);
}


/*----------------------------------------------------------------*/
/*--- map_segments :: WordFM SegmentID Segment*                ---*/
/*--- the DAG of thread segments                               ---*/
/*----------------------------------------------------------------*/

static void segments__generate_vcg ( void ); /* fwds */

/*--------------- to do with Vector Timestamps ---------------*/

/* Vector Timestamp = XArray* ScalarTS */

static Bool is_sane_VTS ( XArray* vts )
{
   UWord     i, n;
   ScalarTS  *st1, *st2;
   n = VG_(sizeXA)( vts );
   if (n >= 2) {
      for (i = 0; i < n-1; i++) {
         st1 = VG_(indexXA)( vts, i );
         st2 = VG_(indexXA)( vts, i+1 );
         if (st1->thrUID >= st2->thrUID)
            return False;
         if (st1->tym == 0 || st2->tym == 0)
            return False;
      }
   }
   return True;
}

static XArray* new_VTS ( void ) {
   return VG_(newXA)( hg_zalloc, hg_free, sizeof(ScalarTS) );
}
static XArray* singleton_VTS ( Thread* thr, UInt tym ) {
   ScalarTS st;
   XArray*  vts;
   tl_assert(thr);
   tl_assert(tym >= 1);
   vts = new_VTS();
   tl_assert(vts);
   st.thrUID = thr->threadUID;
   st.tym = tym;
   VG_(addToXA)( vts, &st );
   return vts;
}


static Bool cmpGEQ_VTS ( XArray* a, XArray* b )
{
   // TODO: this is expensive!
   Word     ia, ib, useda, usedb;
   UInt     tyma, tymb;
   Int      thrUID;
   ScalarTS *tmpa, *tmpb;

   Bool all_leq = True;
   Bool all_geq = True;

   tl_assert(a);
   tl_assert(b);
   useda = VG_(sizeXA)( a );
   usedb = VG_(sizeXA)( b );

   ia = ib = 0;

   while (1) {

      /* This logic is to enumerate triples (thr, tyma, tymb) drawn
         from a and b in order, where thr is the next threadUID
         occurring in either a or b, and tyma/b are the relevant
         scalar timestamps, taking into account implicit zeroes. */
      tl_assert(ia >= 0 && ia <= useda);
      tl_assert(ib >= 0 && ib <= usedb);
      tmpa = tmpb = NULL;

      if (ia == useda && ib == usedb) {
         /* both empty - done */
         break;
      }
      else
      if (ia == useda && ib != usedb) {
         /* a empty, use up b */
         tmpb = VG_(indexXA)( b, ib );
         thrUID = tmpb->thrUID;
         tyma = 0;
         tymb = tmpb->tym;
         ib++;
      }
      else
      if (ia != useda && ib == usedb) {
         /* b empty, use up a */
         tmpa = VG_(indexXA)( a, ia );
         thrUID = tmpa->thrUID;
         tyma = tmpa->tym;
         tymb = 0;
         ia++;
      }
      else {
         /* both not empty; extract lowest-threadUID'd triple */
         tmpa = VG_(indexXA)( a, ia );
         tmpb = VG_(indexXA)( b, ib );
         if (tmpa->thrUID < tmpb->thrUID) {
            /* a has the lowest unconsidered threadUID */
            thrUID = tmpa->thrUID;
            tyma = tmpa->tym;
            tymb = 0;
            ia++;
         }
         else
         if (tmpa->thrUID > tmpb->thrUID) {
            /* b has the lowest unconsidered threadUID */
            thrUID = tmpb->thrUID;
            tyma = 0;
            tymb = tmpb->tym;
            ib++;
         } else {
            /* they both next mention the same threadUID */
            tl_assert(tmpa->thrUID == tmpb->thrUID);
            thrUID = tmpa->thrUID; /* == tmpb->thrId */
            tyma = tmpa->tym;
            tymb = tmpb->tym;
            ia++;
            ib++;
         }
      }

      /* having laboriously determined (thr, tyma, tymb), do something
         useful with it. */
      if (tyma < tymb)
         all_geq = False;
      if (tyma > tymb)
         all_leq = False;
   }

   if (all_leq && all_geq)
      return True; /* PordEQ */
   /* now we know they aren't equal, so either all_leq or all_geq or
      both are false. */
   if (all_leq)
      return False; /* PordLT */
   if (all_geq)
      return True; /* PordGT */
   /* hmm, neither all_geq or all_leq.  This means unordered. */
   return False; /* PordUN */
}


/* Compute max((tick(thra,a),b) into a new XArray.  a and b are
   unchanged.  If neither a nor b supply a value for 'thra',
   assert. */
static
XArray* tickL_and_joinR_VTS ( Thread* thra, XArray* a, XArray* b )
{
   Word     ia, ib, useda, usedb, ticks_found;
   UInt     tyma, tymb, tymMax;
   Int      thrUID;
   XArray*  res;
   ScalarTS *tmpa, *tmpb;

   tl_assert(a);
   tl_assert(b);
   tl_assert(thra);
   useda = VG_(sizeXA)( a );
   usedb = VG_(sizeXA)( b );

   res = new_VTS();
   ia = ib = ticks_found = 0;

   while (1) {

      /* This logic is to enumerate triples (thr, tyma, tymb) drawn
         from a and b in order, where thr is the next threadUID
         occurring in either a or b, and tyma/b are the relevant
         scalar timestamps, taking into account implicit zeroes. */
      tl_assert(ia >= 0 && ia <= useda);
      tl_assert(ib >= 0 && ib <= usedb);
      tmpa = tmpb = NULL;

      if (ia == useda && ib == usedb) {
         /* both empty - done */
         break;
      }
      else
      if (ia == useda && ib != usedb) {
         /* a empty, use up b */
         tmpb = VG_(indexXA)( b, ib );
         thrUID = tmpb->thrUID;
         tyma = 0;
         tymb = tmpb->tym;
         ib++;
      }
      else
      if (ia != useda && ib == usedb) {
         /* b empty, use up a */
         tmpa = VG_(indexXA)( a, ia );
         thrUID = tmpa->thrUID;
         tyma = tmpa->tym;
         tymb = 0;
         ia++;
      }
      else {
         /* both not empty; extract lowest-threadUID'd triple */
         tmpa = VG_(indexXA)( a, ia );
         tmpb = VG_(indexXA)( b, ib );
         if (tmpa->thrUID < tmpb->thrUID) {
            /* a has the lowest unconsidered threadUID */
            thrUID = tmpa->thrUID;
            tyma = tmpa->tym;
            tymb = 0;
            ia++;
         }
         else
         if (tmpa->thrUID > tmpb->thrUID) {
            /* b has the lowest unconsidered threadUID */
            thrUID = tmpb->thrUID;
            tyma = 0;
            tymb = tmpb->tym;
            ib++;
         } else {
            /* they both next mention the same threadUID */
            tl_assert(tmpa->thrUID == tmpb->thrUID);
            thrUID = tmpa->thrUID; /* == tmpb->thrUID */
            tyma = tmpa->tym;
            tymb = tmpb->tym;
            ia++;
            ib++;
         }
      }

      /* having laboriously determined (thr, tyma, tymb), do something
         useful with it. */
      if (thrUID == thra->threadUID) {
         if (tyma > 0) {
            /* VTS 'a' actually supplied this value; it is not a
               default zero.  Do the required 'tick' action. */
            tyma++;
            ticks_found++;
         } else {
            /* 'a' didn't supply this value, so 'b' must have. */
            tl_assert(tymb > 0);
         }
      }
      tymMax = tyma > tymb ? tyma : tymb;
      if (tymMax > 0) {
         ScalarTS st;
         st.thrUID = thrUID;
         st.tym = tymMax;
         VG_(addToXA)( res, &st );
      }

   }

   tl_assert(is_sane_VTS( res ));

   if (thra != NULL) {
      tl_assert(ticks_found == 1);
   } else {
      tl_assert(ticks_found == 0);
   }

   return res;
}


/* Do 'vts[me]++', so to speak.  If 'me' does not have an entry in
   'vts', set it to 1 in the returned VTS. */

static XArray* tick_VTS ( Thread* me, XArray* vts ) {
   ScalarTS* here = NULL;
   ScalarTS  tmp;
   XArray*   res;
   Word      i, n;
   Int       me_UID = me->threadUID;
   tl_assert(me);
   tl_assert(is_sane_VTS(vts));
   if (0) VG_(printf)("tick vts thrno %d szin %d\n", 
                           me_UID, (Int)VG_(sizeXA)(vts) );
   res = new_VTS();
   n = VG_(sizeXA)( vts );
   for (i = 0; i < n; i++) {
      here = VG_(indexXA)( vts, i );
      if (me_UID < here->thrUID) {
         /* We just went past 'me', without seeing it. */
         tmp.thrUID = me_UID;
         tmp.tym = 1;
         VG_(addToXA)( res, &tmp );
         tmp = *here;
         VG_(addToXA)( res, &tmp );
         i++;
         break;
      } 
      else if (me_UID == here->thrUID) {
         tmp = *here;
         tmp.tym++;
         VG_(addToXA)( res, &tmp );
         i++;
         break;
      }
      else /* me > here->thr */ {
         tmp = *here;
         VG_(addToXA)( res, &tmp );
      }
   }
   tl_assert(i >= 0 && i <= n);
   if (i == n && here && here->thrUID < me_UID) {
      tmp.thrUID = me_UID;
      tmp.tym = 1;
      VG_(addToXA)( res, &tmp );
   } else {
      for (/*keepgoing*/; i < n; i++) {
         here = VG_(indexXA)( vts, i );
         tmp = *here;
         VG_(addToXA)( res, &tmp );
      }
   }
   tl_assert(is_sane_VTS(res));
   if (0) VG_(printf)("tick vts thrno %d szou %d\n",
                         me_UID, (Int)VG_(sizeXA)(res) );
   return res;
}

static void show_VTS ( HChar* buf, Int nBuf, XArray* vts ) {
   ScalarTS* st;
   HChar     unit[64];
   Word      i, n;
   Int       avail = nBuf;
   tl_assert(avail > 16);
   buf[0] = '[';
   buf[1] = 0;
   n = VG_(sizeXA)( vts );
   for (i = 0; i < n; i++) {
      tl_assert(avail >= 10);
      st = VG_(indexXA)( vts, i );
      VG_(memset)(unit, 0, sizeof(unit));
      VG_(sprintf)(unit, i < n-1 ? "%d:%ld " : "%d:%ld", st->thrUID, st->tym);
      if (avail < VG_(strlen)(unit) + 10/*let's say*/) {
         VG_(strcat)(buf, " ...]");
         return;
      }
      VG_(strcat)(buf, unit);
      avail -= VG_(strlen)(unit);
   }
   VG_(strcat)(buf, "]");
}

/*------------ handle expected errors -----------------------*/

// See definition of ExpectedError for details. 

static ExpectedError *get_expected_error (Addr ptr)
{
  ExpectedError *expected_error = NULL;
  if (HG_(lookupFM)( map_expected_errors,
                     NULL/*keyP*/, (Word*)&expected_error, (Word)ptr)) {
    tl_assert(expected_error->addr == ptr);
    VG_(printf)("Found expected race: %s:%d %p\t%s\n",
                expected_error->file, expected_error->line, 
                ptr, expected_error->descr);
    return expected_error;
  }
  return NULL;
}

static Bool maybe_set_expected_error (Addr   ptr,
                                      HChar* description, 
                                      HChar* file, 
                                      Int    line, 
                                      Bool   is_benign)
{
   ExpectedError *error = NULL;
//   VG_(printf)("Expected data race: %s:%d %p\t", file, line, ptr);
   if (HG_(lookupFM)( map_expected_errors,
                      NULL/*keyP*/, (Word*)&error, (Word)ptr)) {
//     VG_(printf)("Found\n");
     tl_assert(error);
     return False;
   }
   /* create a new one */
//   VG_(printf)("New\n");
   error = (ExpectedError*)hg_zalloc(sizeof(ExpectedError));
   error->addr = ptr;
   error->detected = False;
   error->is_benign = is_benign;
   error->descr = description;
   error->file  = file; /* need to copy?*/
   error->line  = line;
   tl_assert(error);
   HG_(addToFM)( map_expected_errors, (Word)ptr, (Word)error );
   return True;
}

/*------- mem trace -------------------------------------------*/
/* a client may request to trace certain memory (for better debugging) */
static WordFM *mem_trace_map = NULL;
static void mem_trace_on(UWord mem, ThreadId tid)
{
   Thread *thr =  map_threads_lookup( tid );
   if (clo_trace_level <= 0) return;
   if (!mem_trace_map) {
      mem_trace_map = HG_(newFM)( hg_zalloc, hg_free, NULL);
   }
   HG_(addToFM)(mem_trace_map, mem, mem);
   VG_(message)(Vg_UserMsg, "ENABLED TRACE {{{: %p; S%d/T%d", mem, 
               (Int)thr->csegid,
               (Int)thr->threadUID);
   if (clo_trace_level >= 2) {
      VG_(get_and_pp_StackTrace)( tid, 15);
   }
   VG_(message)(Vg_UserMsg, "}}}");
   VG_(message)(Vg_UserMsg, "");
}

static inline void mem_trace_off(Addr first, Addr last) 
{
   Bool cont = True;
   Addr a;
   if (LIKELY(!mem_trace_map)) return;
   // Turn memory trace off for all addresses in range [first, last].
   while(cont) {
      cont = False;
      HG_(initIterAtFM)(mem_trace_map, first);
      while (HG_(nextIterFM)(mem_trace_map, (Word*)&a, NULL) && a <= last) {
         HG_(delFromFM)(mem_trace_map, NULL, NULL, a);
         cont = True;
         // we deleted one address from the map. Repeat everything again.
         break;
      }
   }
}

static Bool mem_trace_is_on(Word mem)
{
   return mem_trace_map != NULL 
         && HG_(lookupFM)(mem_trace_map, NULL, NULL, mem);
}


/*---------- MU is used as CV -------------------------*/

/* In some cases mutexes are used in such a way that 
 regular lockset algorithms will always report a race even though 
 the code is perfectly synchronized. 
 We can treat such mutexes as pure happens-before detectors do. 
 For example, see test61. 

*/

static WordFM *mu_is_cv_map = NULL;
static void set_mu_is_cv(Word mu, ThreadId tid)
{
   ExeContext *context = NULL;
   // context = VG_(record_ExeContext)(tid, -1/*first_ip_delta*/);
   if (!mu_is_cv_map) {
      mu_is_cv_map = HG_(newFM) (hg_zalloc, hg_free, NULL);
   }
   HG_(addToFM)(mu_is_cv_map, mu, (Word)context);
   // HG_(addToFM)(mu_is_cv_map, mu, (Word)context);
//   VG_(printf)("set_mu_is_cv: %p\n", mu);
//   VG_(get_and_pp_StackTrace)( tid, 15);
}

static void unset_mu_is_cv(Word mu)
{
   if (mu_is_cv_map) {
      HG_(delFromFM)(mu_is_cv_map, NULL, NULL, mu);
   }
}

static Bool mu_is_cv(Word mu)
{
   ExeContext *context;
   Word       w;
   Bool res = False;
   if (clo_pure_happens_before) return True;
   
   res = mu_is_cv_map != NULL
         && HG_(lookupFM)(mu_is_cv_map, &w, (Word*)&context, mu);

   if (res && context) {
      VG_(printf)("mu_is_cv: ");
      VG_(pp_ExeContext)(context);
   }

   return res;
}


/*------- PCQ (aka ProducerConsumerQueue, Message queue) ------ */
/*
  Producer-consumer queue (aka Message queue) creates 
  happens-before relation. 
  Put() is like posting a semaphore and 
  Get() is like waiting on that semaphore. 

  When Get() is called, helgrind has to find the corresponding Put().
  Current implementation will work only for FIFO queues. 

  For each PCQ we maintain a structure that contains the number 
  of puts and the number of gets. 
  The n-th Put() corresponds to n-th Get(). 


  TODO:
  Currently we reuse semaphore routines evh__HG_POSIX_SEM_*.  
  It's better to have separate implementations for Put()/Get(). 

*/
typedef struct {
   Word  client_pcq; // just for consistency checking. 
   Word  n_puts;
   Word  n_gets; 
} PCQ;

static WordFM *pcq_map = NULL; // WordFM client_pcq my_pcq

// Create PCQ for client_pcq. Should be called 
// when the client creates its PCQ. 
static void pcq_create(Word client_pcq)
{
   PCQ *my_pcq;
   if (pcq_map == NULL) {
      // first time init. 
      pcq_map = HG_(newFM)( hg_zalloc, hg_free, NULL);
      tl_assert(pcq_map != NULL);
   }

   my_pcq = (PCQ*) hg_zalloc(sizeof(PCQ));
   my_pcq->client_pcq = client_pcq;
   my_pcq->n_puts     = 0;
   my_pcq->n_gets     = 0;

   tl_assert(!HG_(lookupFM)(pcq_map, NULL, NULL, client_pcq));
   HG_(addToFM)(pcq_map, client_pcq, (Word)my_pcq);
}

// Destroy PCQ (called when client PCQ is destroyed). 
static void pcq_destroy(Word client_pcq)
{
   PCQ *my_pcq;
   Word old_client_pcq;
   Bool found = HG_(delFromFM)(pcq_map, &old_client_pcq, 
                               (Word*)&my_pcq, client_pcq);
   tl_assert(found == True);
   tl_assert(old_client_pcq == client_pcq);
   tl_assert(my_pcq->client_pcq == client_pcq);
}

// fwds
static void evh__HG_POSIX_SEM_WAIT_POST ( ThreadId tid, void* sem );
static void evh__HG_POSIX_SEM_POST_PRE ( ThreadId tid, void* sem );

// Handle PCQ::Put().
static void pcq_put(ThreadId tid, Word client_pcq)
{
   PCQ *my_pcq;
   Word old_client_pcq;
   Bool found = HG_(lookupFM)(pcq_map, &old_client_pcq, 
                              (Word*)&my_pcq, client_pcq);
   tl_assert(found == True);
   tl_assert(old_client_pcq == client_pcq);
   tl_assert(my_pcq->client_pcq == client_pcq);

   evh__HG_POSIX_SEM_POST_PRE(tid, 
                              (void*)((client_pcq << 5) ^ my_pcq->n_puts)
                              );
   my_pcq->n_puts++;
}

// Handle PCQ::Get(). 
static void pcq_get(ThreadId tid, Word client_pcq)
{
   PCQ *my_pcq;
   Word old_client_pcq;
   Bool found = HG_(lookupFM)(pcq_map, &old_client_pcq, 
                              (Word*)&my_pcq, client_pcq);
   tl_assert(found == True);
   tl_assert(old_client_pcq == client_pcq);
   tl_assert(my_pcq->client_pcq == client_pcq);

   evh__HG_POSIX_SEM_WAIT_POST(tid,
                              (void*)((client_pcq << 5) ^ my_pcq->n_gets)
                               );
   my_pcq->n_gets++;
}



/*------------ searching the happens-before graph ------------*/

static UWord stats__hbefore_queries   = 0; // total # queries
static UWord stats__hbefore_hits      = 0; // hits at hash table
static UWord stats__hbefore_gsearches = 0; // # searches in graph
static UWord stats__hbefore_gsearchFs = 0; // # fast searches in graph
static UWord stats__hbefore_invals    = 0; // # cache invals
static UWord stats__hbefore_stk_hwm   = 0; // stack high water mark

/* Running marker for depth-first searches */
/* NOTE: global variable */
static UInt dfsver_current = 0;

/* A stack of possibly-unexplored nodes used in the depth first search */
/* NOTE: global variable */
static XArray* dfsver_stack = NULL;

// FIXME: check this - is it really correct?
__attribute__((noinline))
static Bool happens_before_do_dfs_from_to ( Segment* src, Segment* dst )
{
   Segment* here;
   Word     ssz;

   /* begin SPEEDUP HACK -- the following can safely be omitted */
   /* fast track common case, without favouring either the
      ->prev or ->other links */
   tl_assert(src);
   tl_assert(dst);
   if ((src->prev && src->prev == dst)
       || (src->other && src->other == dst)) {
      stats__hbefore_gsearchFs++;
      return True;
   }
   /* end SPEEDUP HACK */

   /* empty out the stack */
   tl_assert(dfsver_stack);
   VG_(dropTailXA)( dfsver_stack, VG_(sizeXA)( dfsver_stack ));
   tl_assert(VG_(sizeXA)( dfsver_stack ) == 0);

   /* push starting point */
   (void) VG_(addToXA)( dfsver_stack, &src );

   while (True) {
      /* While the stack is not empty, pop the next node off it and
         consider. */
      ssz = VG_(sizeXA)( dfsver_stack );
      tl_assert(ssz >= 0);
      if (ssz == 0)
         return False; /* stack empty ==> no path from src to dst */

      if (UNLIKELY( ((UWord)ssz) > stats__hbefore_stk_hwm ))
         stats__hbefore_stk_hwm = (UWord)ssz;

      /* here = pop(stack) */
      here = *(Segment**) VG_(indexXA)( dfsver_stack, ssz-1 );
      VG_(dropTailXA)( dfsver_stack, 1 );

     again:
      /* consider the node 'here' */
      if (here == dst)
         return True; /* found a path from src and dst */

      /* have we been to 'here' before? */
      tl_assert(here->dfsver <= dfsver_current);
      if (here->dfsver == dfsver_current)
         continue; /* We've been 'here' before - node is not interesting*/

      /* Mark that we've been here */
      here->dfsver = dfsver_current;

      /* Now push both children on the stack */

      /* begin SPEEDUP hack -- the following can safely be omitted */
      /* idea is, if there is exactly one child, avoid the overhead of
         pushing it on the stack and immediately popping it off again.
         Kinda like doing a tail-call. */
      if (here->prev && !here->other) {
         here = here->prev;
         goto again;
      }
      if (here->other && !here->prev) {
         here = here->other;
         goto again;
      }
      /* end of SPEEDUP HACK */

      /* Push all available children on stack.  From some quick
         experimentation it seems like exploring ->other first leads
         to lower maximum stack use, although getting repeatable
         results is difficult. */
      if (here->prev)
         (void) VG_(addToXA)( dfsver_stack, &(here->prev) );
      if (here->other)
         (void) VG_(addToXA)( dfsver_stack, &(here->other) );
   }
}

__attribute__((noinline))
static Bool happens_before_wrk ( Segment* seg1, Segment* seg2 )
{
   Bool reachable;

   { static Int nnn = 0;
     if (SHOW_EXPENSIVE_STUFF && (nnn++ % 1000) == 0)
        VG_(printf)("happens_before_wrk: %d\n", nnn);
   }

   /* Now the question is, is there a chain of pointers through the
      .prev and .other fields, that leads from seg2 back to seg1 ? */
   tl_assert(dfsver_current < 0xFFFFFFFF);
   dfsver_current++;
   
   if (dfsver_stack == NULL) {
     dfsver_stack = VG_(newXA)( hg_zalloc, hg_free, sizeof(Segment*) );
     tl_assert(dfsver_stack);
   }

   reachable = happens_before_do_dfs_from_to( seg2, seg1 );

   return reachable;
}


/*--------------- the happens_before hash table  ---------------*/

// HBEFORE__N_HTABLE should be a prime number.  But not a large prime
// number, as that just causes D1 misses and slows things down.
// #define HBEFORE__N_HTABLE 104729
// #define HBEFORE__N_HTABLE 399989
// #define HBEFORE__N_HTABLE 49999
// #define HBEFORE__N_HTABLE 19997
// #define HBEFORE__N_HTABLE 9973
#define HBEFORE__N_HTABLE 4999

// Simple closed hash table with prime size. 
// Each entry is a 64-bit value: 
// bits 0-31:  SegmentID-2
// bits 32-62: SegmentID-1
// bit 63:     happens-before(SegmentID-1, SegmentID-2)

static 
   struct { Bool res; SegmentID segid1; SegmentID segid2; } 
   hbefore__hash_table[HBEFORE__N_HTABLE];

static void hbefore__invalidate_htable ( void )
{  
   stats__hbefore_invals++;
   VG_(memset)(hbefore__hash_table, 0, sizeof(hbefore__hash_table));
}

static inline UInt ROL32 ( UInt w, Int n )
{
   w = (w << n) | (w >> (32-n));
   return w;
}

static Bool happens_before ( SegmentID segid1, SegmentID segid2 )
{
   // example call: happens_before(prevS, currS)
   Bool    hbG, hbV;
   Segment *seg1, *seg2;
   UInt    hash;
   tl_assert(sizeof(SegmentID) == sizeof(UInt));
   tl_assert(SEG_id_is_sane(segid1));
   tl_assert(SEG_id_is_sane(segid2));
   tl_assert(segid1 != segid2);
   stats__hbefore_queries++;
   hash = ROL32(segid1,19) ^ ROL32(segid2,13);
   /* make sure % is done at 32 bits, since % on a 64-bit
      value on a 32-bit machine is very expensive. */
   hash %= HBEFORE__N_HTABLE;
   { // try hash table
     // Hmm, % on ULong is OK on 64-bit machine (32ish cycles on Core2) 
     // but really bad on 32-bit (there is a call to umoddi3 and that
     // can be many tens of instructions)
     if (hbefore__hash_table[hash].segid1 == segid1 
         && hbefore__hash_table[hash].segid2 == segid2) {
        stats__hbefore_hits++;
        return hbefore__hash_table[hash].res;
     }
   }

   /* Not found.  Search the graph and add an entry to the cache. */
   stats__hbefore_gsearches++;

   seg1 = SEG_get(segid1);
   seg2 = SEG_get(segid2);
   tl_assert(is_sane_Segment(seg1));
   tl_assert(is_sane_Segment(seg2));
   tl_assert(seg1 != seg2);
   tl_assert(seg1->vts);
   tl_assert(seg2->vts);

   hbV = cmpGEQ_VTS( seg2->vts, seg1->vts );
   if (clo_sanity_flags & SCE_HBEFORE) {
      /* Crosscheck the vector-timestamp comparison result against that
         obtained from the explicit graph approach.  Can be very
         slow. */
      hbG = happens_before_wrk( seg1, seg2 );
   } else {
      /* Assume the vector-timestamp comparison result is correct, and
         use it as-is. */
      hbG = hbV;
   }

   if (hbV != hbG) {
      VG_(printf)("seg1 %p  seg2 %p  hbV %d  hbG %d\n", 
                  seg1,seg2,(Int)hbV,(Int)hbG);
      segments__generate_vcg();
   }
   tl_assert(hbV == hbG);

   { // remember the results in the hash table 
     hbefore__hash_table[hash].segid1 = segid1;
     hbefore__hash_table[hash].segid2 = segid2;
     hbefore__hash_table[hash].res = hbG;
   }
   if (0)
   VG_(printf)("hb %d %d\n", (Int)segid1, (Int)segid2);
   return hbG;
}


/*--------------- generating .vcg output ---------------*/

static void segments__generate_vcg ( void )
{
#define PFX "xxxxxx"
   /* Edge colours:
         Black  -- the chain of .prev links
         Green  -- thread creation, link to parent
         Red    -- thread exit, link to exiting thread
         Yellow -- signal edge
         Pink   -- semaphore-up edge
   */
   UInt i;
   HChar vtsstr[128];
   VG_(printf)(PFX "graph: { title: \"Segments\"\n");
   VG_(printf)(PFX "orientation: top_to_bottom\n");
   VG_(printf)(PFX "height: 900\n");
   VG_(printf)(PFX "width: 500\n");
   VG_(printf)(PFX "x: 20\n");
   VG_(printf)(PFX "y: 20\n");
   VG_(printf)(PFX "color: lightgrey\n");
   for (i = 1; i < SegmentArray.size; i++) {
      Segment* seg = SEG_get(i);

      VG_(printf)(PFX "node: { title: \"%p\" color: lightcyan "
                  "textcolor: darkgreen label: \"Seg %p\\n", 
                  seg, seg);
      if (seg->thr->threadUID == 1) {
         VG_(printf)("ROOT_THREAD");
      } else {
         VG_(printf)("Thr# %d", seg->thr->threadUID);
      }

      if (clo_gen_vcg >= 2) {
         show_VTS( vtsstr, sizeof(vtsstr)-1, seg->vts );
         vtsstr[sizeof(vtsstr)-1] = 0;
         VG_(printf)("\\n%s", vtsstr);
      }

      VG_(printf)("\" }\n", vtsstr);

      if (seg->prev)
         VG_(printf)(PFX "edge: { sourcename: \"%p\" targetname: \"%p\""
                     "color: black }\n", seg->prev, seg );
      if (seg->other) {
         HChar* colour = "N/A";
         DEBUG_ONLY(switch (seg->other_hint) {
            case 'c': colour = "darkgreen";  break; /* creation */
            case 'j': colour = "red";        break; /* join (exit) */
            case 's': colour = "orange";     break; /* signal */
            case 'S': colour = "pink";       break; /* sem_post->wait */
            case 'u': colour = "cyan";       break; /* unlock */
            default: tl_assert(0);
         })
         VG_(printf)(PFX "edge: { sourcename: \"%p\" targetname: \"%p\""
                     " color: %s }\n", seg->other, seg, colour );
      }
   }
   VG_(printf)(PFX "}\n");
#undef PFX
}


/*----------------------------------------------------------------*/
/*--- map_shmem :: WordFM Addr SecMap                          ---*/
/*--- shadow memory (low level handlers) (shmem__* fns)        ---*/
/*----------------------------------------------------------------*/

static UWord stats__secmaps_search       = 0; // # SM finds
static UWord stats__secmaps_search_slow  = 0; // # SM lookupFMs
static UWord stats__secmaps_allocd       = 0; // # SecMaps issued
static UWord stats__secmap_ga_space_covered = 0; // # ga bytes covered
static UWord stats__secmap_linesZ_allocd = 0; // # CacheLineZ's issued
static UWord stats__secmap_linesZ_bytes  = 0; // .. using this much storage
static UWord stats__secmap_linesF_allocd = 0; // # CacheLineF's issued
static UWord stats__secmap_linesF_bytes  = 0; //  .. using this much storage
static UWord stats__secmap_iterator_steppings = 0; // # calls to stepSMIter
static UWord stats__cache_Z_fetches      = 0; // # Z lines fetched
static UWord stats__cache_Z_wbacks       = 0; // # Z lines written back
static UWord stats__cache_F_fetches      = 0; // # F lines fetched
static UWord stats__cache_F_wbacks       = 0; // # F lines written back
static UWord stats__cache_invals         = 0; // # cache invals
static UWord stats__cache_flushes        = 0; // # cache flushes
static UWord stats__cache_totrefs        = 0; // # total accesses
static UWord stats__cache_totmisses      = 0; // # misses
static ULong stats__cache_make_New_arange = 0; // total arange made New
static ULong stats__cache_make_New_inZrep = 0; // arange New'd on Z reps
static UWord stats__cline_normalises     = 0; // # calls to cacheline_normalise
static UWord stats__cline_read64s        = 0; // # calls to s_m_read64
static UWord stats__cline_read32s        = 0; // # calls to s_m_read32
static UWord stats__cline_read16s        = 0; // # calls to s_m_read16
static UWord stats__cline_read8s         = 0; // # calls to s_m_read8
static UWord stats__cline_write64s       = 0; // # calls to s_m_write64
static UWord stats__cline_write32s       = 0; // # calls to s_m_write32
static UWord stats__cline_write16s       = 0; // # calls to s_m_write16
static UWord stats__cline_write8s        = 0; // # calls to s_m_write8
static UWord stats__cline_set64s         = 0; // # calls to s_m_set64
static UWord stats__cline_set32s         = 0; // # calls to s_m_set32
static UWord stats__cline_set16s         = 0; // # calls to s_m_set16
static UWord stats__cline_set8s          = 0; // # calls to s_m_set8
static UWord stats__cline_get8s          = 0; // # calls to s_m_get8
static UWord stats__cline_copy8s         = 0; // # calls to s_m_copy8
static UWord stats__cline_64to32splits   = 0; // # 64-bit accesses split
static UWord stats__cline_32to16splits   = 0; // # 32-bit accesses split
static UWord stats__cline_16to8splits    = 0; // # 16-bit accesses split
static UWord stats__cline_64to32pulldown = 0; // # calls to pulldown_to_32
static UWord stats__cline_32to16pulldown = 0; // # calls to pulldown_to_16
static UWord stats__cline_16to8pulldown  = 0; // # calls to pulldown_to_8


static SVal shadow_mem_get8 ( Addr a ); /* fwds */

static inline Addr shmem__round_to_SecMap_base ( Addr a ) {
   return a & ~(N_SECMAP_ARANGE - 1);
}
static inline UWord shmem__get_SecMap_offset ( Addr a ) {
   return a & (N_SECMAP_ARANGE - 1);
}

/*--------------- SecMap allocation --------------- */
static inline Bool address_may_be_ignored ( Addr a ); // fwds

static HChar* shmem__bigchunk_next = NULL;
static HChar* shmem__bigchunk_end1 = NULL;

static void* shmem__bigchunk_alloc ( SizeT n )
{
   const SizeT sHMEM__BIGCHUNK_SIZE = 4096 * 256;
   tl_assert(n > 0);
   n = ROUNDUP(n, 16);
   tl_assert(shmem__bigchunk_next <= shmem__bigchunk_end1);
   tl_assert(shmem__bigchunk_end1 - shmem__bigchunk_next
             <= (SSizeT)sHMEM__BIGCHUNK_SIZE);
   if (shmem__bigchunk_next + n > shmem__bigchunk_end1) {
      if (0)
      VG_(printf)("XXXXX bigchunk: abandoning %d bytes\n", 
                  (Int)(shmem__bigchunk_end1 - shmem__bigchunk_next));
      shmem__bigchunk_next = VG_(am_shadow_alloc)( sHMEM__BIGCHUNK_SIZE );
      shmem__bigchunk_end1 = shmem__bigchunk_next + sHMEM__BIGCHUNK_SIZE;
   }
   tl_assert(shmem__bigchunk_next);
   tl_assert( 0 == (((Addr)shmem__bigchunk_next) & (16-1)) );
   tl_assert(shmem__bigchunk_next + n <= shmem__bigchunk_end1);
   shmem__bigchunk_next += n;
   return shmem__bigchunk_next - n;
}

static SecMap* shmem__alloc_SecMap ( void )
{
   Word    i, j;
   SecMap* sm = shmem__bigchunk_alloc( sizeof(SecMap) );
   if (0) VG_(printf)("alloc_SecMap %p\n",sm);
   tl_assert(sm);
   sm->magic       = SecMap_MAGIC;
   sm->mbHasLocks  = False; /* dangerous */
   for (i = 0; i < N_SECMAP_ZLINES; i++) {
      sm->linesZ[i].dict[0] = SHVAL_New;
      sm->linesZ[i].dict[1] = 0; /* completely invalid SHVAL */
      sm->linesZ[i].dict[2] = 0;
      sm->linesZ[i].dict[3] = 0;
      for (j = 0; j < N_LINE_ARANGE/4; j++)
         sm->linesZ[i].ix2s[j] = 0; /* all reference dict[0] */
   }
   sm->linesF      = NULL;
   sm->linesF_size = 0;
   stats__secmaps_allocd++;
   stats__secmap_ga_space_covered += N_SECMAP_ARANGE;
   stats__secmap_linesZ_allocd += N_SECMAP_ZLINES;
   stats__secmap_linesZ_bytes += N_SECMAP_ZLINES * sizeof(CacheLineZ);
   return sm;
}

typedef struct { Addr gaKey; SecMap* sm; } SMCacheEnt;
static SMCacheEnt smCache[3] = { {1,NULL}, {1,NULL}, {1,NULL} };

static SecMap* shmem__find_SecMap ( Addr ga ) 
{
   SecMap* sm    = NULL;
   Addr    gaKey = shmem__round_to_SecMap_base(ga);
   // Cache
   stats__secmaps_search++;
   if (LIKELY(gaKey == smCache[0].gaKey))
      return smCache[0].sm;
   if (LIKELY(gaKey == smCache[1].gaKey)) {
      SMCacheEnt tmp = smCache[0];
      smCache[0] = smCache[1];
      smCache[1] = tmp;
      return smCache[0].sm;
   }
   if (gaKey == smCache[2].gaKey) {
      SMCacheEnt tmp = smCache[1];
      smCache[1] = smCache[2];
      smCache[2] = tmp;
      return smCache[1].sm;
   }
   // end Cache
   stats__secmaps_search_slow++;
   if (HG_(lookupFM)( map_shmem,
                      NULL/*keyP*/, (Word*)&sm, (Word)gaKey )) {
      tl_assert(sm != NULL);
      smCache[2] = smCache[1];
      smCache[1] = smCache[0];
      smCache[0].gaKey = gaKey;
      smCache[0].sm    = sm;
   } else {
      tl_assert(sm == NULL);
   }
   return sm;
}

static SecMap* shmem__find_or_alloc_SecMap ( Addr ga )
{
   SecMap* sm = shmem__find_SecMap ( ga );
   if (LIKELY(sm)) {
      return sm;
   } else {
      /* create a new one */
      Addr gaKey = shmem__round_to_SecMap_base(ga);
      sm = shmem__alloc_SecMap();
      tl_assert(sm);
      HG_(addToFM)( map_shmem, (Word)gaKey, (Word)sm );
      return sm;
   }
}


/*--------------- cache management/lookup --------------- */

/*--------------- misc --------------- */

static Bool shmem__get_mbHasLocks ( Addr a )
{
   SecMap* sm = shmem__find_SecMap ( a );
   if (sm) {
      /* Found */
      return sm->mbHasLocks;
   } else {
      return False;
   }
}

static void shmem__set_mbHasLocks ( Addr a, Bool b )
{
   SecMap* sm;
   Addr    aKey;

   // avoid creating a SecMap for memory that we ignore.
   if (UNLIKELY(b == False && clo_ignore_n != 1 && address_may_be_ignored(a)))
      return;

   tl_assert(b == False || b == True);
   aKey = shmem__round_to_SecMap_base(a);

   sm = shmem__find_or_alloc_SecMap(a);
   tl_assert(sm);
   sm->mbHasLocks = b;
}


/*----------------------------------------------------------------*/
/*--- Sanity checking the data structures                      ---*/
/*----------------------------------------------------------------*/

static UWord stats__sanity_checks = 0;

static Bool is_sane_CacheLine ( CacheLine* cl ); /* fwds */
static Bool cmpGEQ_VTS ( XArray* a, XArray* b ); /* fwds */
static void laog__sanity_check ( Char* who ); /* fwds */

/* REQUIRED INVARIANTS:

   Thread vs Segment/Lock/SecMaps

      for each t in Threads {

         // Thread.lockset: each element is really a valid Lock

         // Thread.lockset: each Lock in set is actually held by that thread
         for lk in Thread.lockset 
            lk == LockedBy(t)

         // Thread.csegid is a valid SegmentID
         // and the associated Segment has .thr == t

      }

      all thread Locksets are pairwise empty under intersection
      (that is, no lock is claimed to be held by more than one thread)
      -- this is guaranteed if all locks in locksets point back to their
      owner threads

   Lock vs Thread/Segment/SecMaps

      for each entry (gla, la) in map_locks
         gla == la->guest_addr

      for each lk in Locks {

         lk->tag is valid
         lk->guest_addr does not have shadow state NoAccess
         if lk == LockedBy(t), then t->lockset contains lk
         if lk == UnlockedBy(segid) then segid is valid SegmentID
             and can be mapped to a valid Segment(seg)
             and seg->thr->lockset does not contain lk
         if lk == UnlockedNew then (no lockset contains lk)

         secmaps for lk has .mbHasLocks == True

      }

   Segment vs Thread/Lock/SecMaps

      the Segment graph is a dag (no cycles)
      all of the Segment graph must be reachable from the segids
         mentioned in the Threads

      for seg in Segments {

         seg->thr is a sane Thread

      }

   SecMaps vs Segment/Thread/Lock

      for sm in SecMaps {

         sm properly aligned
         if any shadow word is ShR or ShM then .mbHasShared == True

         for each Excl(segid) state
            SEG_get maps to a sane Segment(seg)
         for each ShM/ShR(tsetid,lsetid) state
            each lk in lset is a valid Lock
            each thr in tset is a valid thread, which is non-dead

      }
*/


/* Return True iff 'thr' holds 'lk' in some mode. */
static Bool thread_is_a_holder_of_Lock ( Thread* thr, Lock* lk )
{
   return HG_(elemBag)( &lk->heldBy, (Word)thr ) > 0;
}

/* Sanity check Threads, as far as possible */
__attribute__((noinline))
static void threads__sanity_check ( Char* who )
{
#define BAD(_str) do { how = (_str); goto bad; } while (0)
   Char*     how = "no error";
   Thread*   thr;
   WordSetID wsA, wsW;
   UWord*    ls_words;
   Word      ls_size, i;
   Lock*     lk;
   Segment*  seg;
   for (thr = admin_threads; thr; thr = thr->admin) {
      if (!is_sane_Thread(thr)) BAD("1");
      wsA = thr->locksetA;
      wsW = thr->locksetW;
      // locks held in W mode are a subset of all locks held
      if (!HG_(isSubsetOf)( univ_lsets, wsW, wsA )) BAD("7");
      HG_(getPayloadWS)( &ls_words, &ls_size, univ_lsets, wsA );
      for (i = 0; i < ls_size; i++) {
         lk = (Lock*)ls_words[i];
         // Thread.lockset: each element is really a valid Lock
         if (!is_sane_LockN(lk)) BAD("2");
         // Thread.lockset: each Lock in set is actually held by that
         // thread
         if (!thread_is_a_holder_of_Lock(thr,lk)) BAD("3");
         // Thread.csegid is a valid SegmentID
         if (!SEG_id_is_sane(thr->csegid)) BAD("4");
         // and the associated Segment has .thr == t
         seg = SEG_maybe_get(thr->csegid);
         if (!is_sane_Segment(seg)) BAD("5");
         if (seg->thr != thr) BAD("6");
      }
   }
   return;
  bad:
   VG_(printf)("threads__sanity_check: who=\"%s\", bad=\"%s\"\n", who, how);
   tl_assert(0);
#undef BAD
}


/* Sanity check Locks, as far as possible */
__attribute__((noinline))
static void locks__sanity_check ( Char* who )
{
#define BAD(_str) do { how = (_str); goto bad; } while (0)
   Char*     how = "no error";
   Addr      gla;
   Lock*     lk;
   UWord     i;
   // # entries in admin_locks == # entries in map_locks
   for (i = 0, lk = admin_locks;  lk;  i++, lk = lk->admin)
      ;
   if (i != HG_(sizeFM)(map_locks)) BAD("1");
   // for each entry (gla, lk) in map_locks
   //      gla == lk->guest_addr
   HG_(initIterFM)( map_locks );
   while (HG_(nextIterFM)( map_locks,
                           (Word*)&gla, (Word*)&lk )) {
      if (lk->guestaddr != gla) BAD("2");
   }
   HG_(doneIterFM)( map_locks );
   // scan through admin_locks ...
   for (lk = admin_locks; lk; lk = lk->admin) {
      // lock is sane.  Quite comprehensive, also checks that
      // referenced (holder) threads are sane.
      if (!is_sane_LockN(lk)) BAD("3");
      // map_locks binds guest address back to this lock
      if (lk != map_locks_maybe_lookup(lk->guestaddr)) BAD("4");
      // lk->guest_addr does not have shadow state NoAccess
      // FIXME: this could legitimately arise from a buggy guest
      // that attempts to lock in (eg) freed memory.  Detect this
      // and warn about it in the pre/post-mutex-lock event handler.
      //if (is_SHVAL_NoAccess(shadow_mem_get8(lk->guestaddr))) BAD("5");
      // look at all threads mentioned as holders of this lock.  Ensure
      // this lock is mentioned in their locksets.
      if (!HG_(isEmptyBag)( &lk->heldBy )) {
         Thread* thr;
         Word    count;
         HG_(initIterBag)( &lk->heldBy );
         while (HG_(nextIterBag)( &lk->heldBy, 
                                  (Word*)&thr, &count )) {
            // is_sane_LockN above ensures these
            tl_assert(count >= 1);
            tl_assert(is_sane_Thread(thr));
            if (!HG_(elemWS)(univ_lsets, thr->locksetA, (Word)lk)) 
               BAD("6");
            // also check the w-only lockset
            if (lk->heldW 
                && !HG_(elemWS)(univ_lsets, thr->locksetW, (Word)lk)) 
               BAD("7");
            if ((!lk->heldW)
                && HG_(elemWS)(univ_lsets, thr->locksetW, (Word)lk)) 
               BAD("8");
         }
         HG_(doneIterBag)( &lk->heldBy );
      } else {
         /* lock not held by anybody */
         if (lk->heldW) BAD("9"); /* should be False if !heldBy */
         // since lk is unheld, then (no lockset contains lk)
         // hmm, this is really too expensive to check.  Hmm.
      }
      // secmaps for lk has .mbHasLocks == True
      if (!shmem__get_mbHasLocks(lk->guestaddr)) BAD("10");
   }

   return;
  bad:
   VG_(printf)("locks__sanity_check: who=\"%s\", bad=\"%s\"\n", who, how);
   tl_assert(0);
#undef BAD
}


/* Sanity check Segments, as far as possible */
__attribute__((noinline))
static void segments__sanity_check ( Char* who )
{
#define BAD(_str) do { how = (_str); goto bad; } while (0)
   Char*    how = "no error";
   UInt      i;
   Segment* seg;
   // FIXME
   //   the Segment graph is a dag (no cycles)
   //   all of the Segment graph must be reachable from the segids
   //      mentioned in the Threads
   // for seg in Segments {
   for (i = 1; i < SegmentArray.size; i++) {
      seg = SEG_get(i);
      if (!is_sane_Segment(seg)) BAD("2");
      if (!is_sane_Thread(seg->thr)) BAD("3");
      if (!seg->vts) BAD("4");
      if (seg->prev && seg->prev->vts
          && !cmpGEQ_VTS(seg->vts, seg->prev->vts))
         BAD("5");
      if (seg->other && seg->other->vts
          && !cmpGEQ_VTS(seg->vts, seg->other->vts))
         BAD("6");
   }
   return;
  bad:
   VG_(printf)("segments__sanity_check: who=\"%s\", bad=\"%s\"\n", 
               who, how);
   tl_assert(0);
#undef BAD
}


/* Sanity check shadow memory, as far as possible */
static Int cmp_Addr_for_ssort ( void* p1, void* p2 ) {
   Addr a1 = *(Addr*)p1;
   Addr a2 = *(Addr*)p2;
   if (a1 < a2) return -1;
   if (a1 > a2) return 1;
   return 0;
}
__attribute__((noinline))
static void shmem__sanity_check ( Char* who )
{
#define BAD(_str) do { how = (_str); goto bad; } while (0)
   Char*   how = "no error";
   SecMap* sm;
   UWord   smga, j, i, n_valid_tags, ws_size;
   UWord*  ws_words;
   Addr*   valid_tags;
   HG_(initIterFM)( map_shmem );
   // for sm in SecMaps {
   while (HG_(nextIterFM)( map_shmem, &smga, (Word*)&sm )) {
      SecMapIter itr;
      SVal*      sv_p = NULL;
      //Bool       mbHasShared = False;
      //Bool       allNoAccess = True;
      if (!is_sane_SecMap(sm)) BAD("1");
      // sm properly aligned
      if (smga != shmem__round_to_SecMap_base(smga)) BAD("2");
      // if any shadow word is ShR or ShM then .mbHasShared == True
      initSecMapIter( &itr );
      while (stepSecMapIter( &sv_p, &itr, sm )) {
         SVal sv = *sv_p;
         //if (is_SHVAL_Shared(sv)) 
         //   mbHasShared = True;
         //if (!is_SHVAL_NoAccess(sv))
         //   allNoAccess = False;

         if (is_SHVAL_RM(sv)) {
            LockSet    LS = get_SHVAL_LS(sv);
            SegmentSet SS = get_SHVAL_SS(sv);
            // check segment set
            for (j = 0; j < SS_get_size(SS); j++) {
               SegmentID id = SS_get_element(SS, j);
               if (!SEG_id_is_sane(id)) BAD("3");
               if (!is_sane_Segment(SEG_get(id))) BAD("4");
            }
            if (!SS_is_singleton(SS)) {
               if (!HG_(plausibleWS)( univ_ssets, SS )) BAD("5");
               if (!HG_(saneWS_SLOW)( univ_ssets, SS )) BAD("6");
               if (HG_(cardinalityWS)( univ_ssets, SS ) < 2) BAD("7");
            }
            // check lock set 
            if (!HG_(plausibleWS)( univ_lsets, LS )) BAD("8");
            if (!HG_(saneWS_SLOW)( univ_lsets, LS )) BAD("9");
            HG_(getPayloadWS)( &ws_words, &ws_size, univ_lsets, LS );
            for (j = 0; j < ws_size; j++) {
               Lock* lk = (Lock*)ws_words[j];
               // for each ShM/ShR(tsetid,lsetid) state
               // each lk in LS is a valid Lock
               if (!is_sane_LockN(lk)) BAD("10");
            }
         }
         else if (is_SHVAL_New(sv) || is_SHVAL_Ignore(sv)) {
            /* nothing to check */
         }
         else {
            /* bogus shadow mem value */
            BAD("12");
         }
      } /* iterating over a SecMap */
      // Check essential safety property
      //if (mbHasShared && !sm->mbHasShared) BAD("13");
      // This is optional - check that destroyed memory has its hint
      // bits cleared.  NB won't work properly unless full, eager
      // GCing of SecMaps is implemented
      //if (allNoAccess && sm->mbHasLocks) BAD("13a");
   }
   HG_(doneIterFM)( map_shmem );

   // check the cache
   valid_tags   = hg_zalloc(N_WAY_NENT * sizeof(Addr));
   n_valid_tags = 0;
   tl_assert(valid_tags);
   for (i = 0; i < N_WAY_NENT; i++) {
      CacheLine* cl;
      Addr       tag; 
      /* way0, dude */
      cl  = &cache_shmem.lyns0[i];
      tag =  cache_shmem.tags0[i];
      if (tag != 1) {
         if (!is_valid_scache_tag(tag)) BAD("14-0");
         if (!is_sane_CacheLine(cl)) BAD("15-0");
         /* A valid tag should be of the form 
            X---X line_number:N_WAY_BITS 0:N_LINE_BITS */
         if (tag & (N_LINE_ARANGE-1)) BAD("16-0");
         if ( i != ((tag >> N_LINE_BITS) & (N_WAY_NENT-1)) ) BAD("16-1");
         valid_tags[n_valid_tags++] = tag;
      }
   }
   tl_assert(n_valid_tags <= N_WAY_NENT);
   if (n_valid_tags > 1) {
      /* Check that the valid tags are unique */
      VG_(ssort)( valid_tags, n_valid_tags, sizeof(Addr), cmp_Addr_for_ssort );
      for (i = 0; i < n_valid_tags-1; i++) {
         if (valid_tags[i] >= valid_tags[i+1])
            BAD("16-2");
      }
   }
   hg_free(valid_tags);
   return;
  bad:
   VG_(printf)("shmem__sanity_check: who=\"%s\", bad=\"%s\"\n", who, how);
   tl_assert(0);
#undef BAD
}

static void all_except_Locks__sanity_check ( Char* who ) {
   stats__sanity_checks++;
   if (0) VG_(printf)("all_except_Locks__sanity_check(%s)\n", who);
   threads__sanity_check(who);
   segments__sanity_check(who);
   shmem__sanity_check(who);
   laog__sanity_check(who);
}
static void all__sanity_check ( Char* who ) {
   all_except_Locks__sanity_check(who);
   locks__sanity_check(who);
}


/*----------------------------------------------------------------*/
/*--- the core memory state machine (msm__* functions)         ---*/
/*----------------------------------------------------------------*/

static UWord stats__msm_BHL_hack     = 0;
static UWord stats__msm_Ignore       = 0;
static UWord stats__msm_R_to_R       = 0;
static UWord stats__msm_R_to_M       = 0;
static UWord stats__msm_M_to_R       = 0;
static UWord stats__msm_M_to_M       = 0;
static UWord stats__msm_New_to_M     = 0;
static UWord stats__msm_New_to_R     = 0;
static UWord stats__msm_oldSS_single = 0;
static UWord stats__msm_oldSS_multi  = 0;
static UWord stats__msm_oldSS_multi_shortcut  = 0;
static UWord stats__msm_oldSS_multi_add       = 0;
static UWord stats__msm_oldSS_multi_del       = 0;

/* fwds */
static Bool record_error_Race ( Thread* thr, 
                                Addr data_addr, Bool isWrite, Int szB,
                                SVal old_sv, SVal new_sv,
                                ExeContext* mb_lastlock );

static void record_error_FreeMemLock ( Thread* thr, Lock* lk );

static void record_error_UnlockUnlocked ( Thread*, Lock* );
static void record_error_UnlockForeign  ( Thread*, Thread*, Lock* );
static void record_error_UnlockBogus    ( Thread*, Addr );
static void record_error_PthAPIerror    ( Thread*, HChar*, Word, HChar* );
static void record_error_LockOrder      ( Thread*, Addr, Addr,
                                                   ExeContext*, ExeContext* );

static void record_error_Misc ( Thread*, HChar* );
static void announce_one_thread ( Thread* thr ); /* fwds */

// KCC: If you agree with the new scheme of handling BHL, 
// KCC: add_BHL/del_BHL could be deleted completely. 
// KCC: Now these functions are commented out to avoid compiler warnings.
//static WordSetID add_BHL ( WordSetID lockset ) {
//   return HG_(addToWS)( univ_lsets, lockset, (Word)__bus_lock_Lock );
//}
static inline WordSetID del_BHL ( WordSetID lockset ) {
   return HG_(delFromWS)( univ_lsets, lockset, (Word)__bus_lock_Lock );
}


/* Last-lock-lossage records.  This mechanism exists to help explain
   to programmers why we are complaining about a race.  The idea is to
   monitor all lockset transitions.  When a previously nonempty
   lockset becomes empty, the lock(s) that just disappeared (the
   "lossage") are the locks that have consistently protected the
   location (ga_of_access) in question for the longest time.  Most of
   the time the lossage-set is a single lock.  Because the
   lossage-lock is the one that has survived longest, there is there
   is a good chance that it is indeed the lock that the programmer
   intended to use to protect the location.

   Note that we cannot in general just look at the lossage set when we
   see a transition to ShM(...,empty-set), because a transition to an
   empty lockset can happen arbitrarily far before the point where we
   want to report an error.  This is in the case where there are many
   transitions ShR -> ShR, all with an empty lockset, and only later
   is there a transition to ShM.  So what we want to do is note the
   lossage lock at the point where a ShR -> ShR transition empties out
   the lockset, so we can present it later if there should be a
   transition to ShM.

   So this function finds such transitions.  For each, it associates
   in ga_to_lastlock, the guest address and the lossage lock.  In fact
   we do not record the Lock* directly as that may disappear later,
   but instead the ExeContext inside the Lock which says where it was
   initialised or first locked.  ExeContexts are permanent so keeping
   them indefinitely is safe.

   A boring detail: the hardware bus lock is not interesting in this
   respect, so we first remove that from the pre/post locksets.
*/

static UWord stats__ga_LL_adds = 0;

static WordFM* ga_to_lastlock = NULL; /* GuestAddr -> ExeContext* */

static __attribute((noinline))
void record_last_lock_lossage ( Addr ga_of_access,
                                WordSetID lset_old, WordSetID lset_new )
{
   Lock* lk;
   Int   card_old, card_new;

   tl_assert(lset_old != lset_new);

   if (0) VG_(printf)("XX1: %d (card %d) -> %d (card %d) %p\n", 
                      (Int)lset_old, 
                      HG_(cardinalityWS)(univ_lsets,lset_old),
                      (Int)lset_new, 
                      HG_(cardinalityWS)(univ_lsets,lset_new),
                      ga_of_access );

   /* If the new lockset is neither empty nor contains merely the bus
      lock (that is, it contains at least one "real" lock), we're not
      interested. */
   if (!HG_(isSingletonOrEmptyWS)(univ_lsets, lset_new, (Word)__bus_lock_Lock))
      return;

   /* This is slow, but at least it's simple.  The bus hardware lock
      just confuses the logic, so remove it from the locksets we're
      considering before doing anything else. */
   lset_new = del_BHL( lset_new );
   
   /* lset_new is now empty */
   card_new = HG_(cardinalityWS)( univ_lsets, lset_new );
   tl_assert(card_new == 0);

   lset_old = del_BHL( lset_old );
   card_old = HG_(cardinalityWS)( univ_lsets, lset_old );

   if (0) VG_(printf)(" X2: %d (card %d) -> %d (card %d)\n",
                      (Int)lset_old, card_old, (Int)lset_new, card_new );

   if (card_old == 0) {
      /* The old lockset was also empty.  Not interesting. */
      return;
   }

   tl_assert(card_old > 0);
   tl_assert(!HG_(isEmptyWS)( univ_lsets, lset_old ));

   /* Now we know we've got a transition from a nonempty lockset to an
      empty one.  So lset_old must be the set of locks lost.  Record
      some details.  If there is more than one element in the lossage
      set, just choose one arbitrarily -- not the best, but at least
      it's simple. */

   lk = (Lock*)HG_(anyElementOfWS)( univ_lsets, lset_old );
   if (0) VG_(printf)("lossage %d %p\n", 
                      HG_(cardinalityWS)( univ_lsets, lset_old), lk );
   if (lk->appeared_at) {
      if (ga_to_lastlock == NULL)
         ga_to_lastlock = HG_(newFM)( hg_zalloc, hg_free, NULL );
      HG_(addToFM)( ga_to_lastlock, ga_of_access, (Word)lk->appeared_at );
      stats__ga_LL_adds++;
   }
}


/* This queries the table (ga_to_lastlock) made by
   record_last_lock_lossage, when constructing error messages.  It
   attempts to find the ExeContext of the allocation or initialisation
   point for the lossage lock associated with 'ga'. */

static ExeContext* maybe_get_lastlock_initpoint ( Addr ga ) 
{
   ExeContext* ec_hint = NULL;
   if (ga_to_lastlock != NULL 
       && HG_(lookupFM)(ga_to_lastlock, 
                        NULL, (Word*)&ec_hint, ga)) {
      tl_assert(ec_hint != NULL);
      return ec_hint;
   } else {
      return NULL;
   }
}


static void msm__show_state_change ( Thread* thr_acc, Addr a, Int szB,
                                     Char howC,
                                     SVal sv_old, SVal sv_new )
{
   ThreadId tid;
   UChar txt_old[100], txt_new[100];
   Char* how = "";
   tl_assert(is_sane_Thread(thr_acc));
   tl_assert(clo_trace_level == 1 || clo_trace_level == 2);
   switch (howC) {
      case 'r': how = "rd"; break;
      case 'w': how = "wr"; break;
      case 'p': how = "pa"; break;
      default: tl_assert(0);
   }
   show_sval(txt_old, sizeof(txt_old), sv_old);
   show_sval(txt_new, sizeof(txt_new), sv_new);
   txt_old[sizeof(txt_old)-1] = 0;
   txt_new[sizeof(txt_new)-1] = 0;
   if (clo_trace_level == 2) {
      /* show everything */
      VG_(message)(Vg_UserMsg, "");
      announce_one_thread( thr_acc );
      VG_(message)(Vg_UserMsg, 
                   "TRACE: %p %s %d thr#%d :: %s --> %s",
                   a, how, szB, thr_acc->threadUID, txt_old, txt_new );
      tid = map_threads_maybe_reverse_lookup(thr_acc);
      if (tid != VG_INVALID_THREADID) {
         VG_(get_and_pp_StackTrace)( tid, 8 );
      }
   } else {
      /* Just print one line */
      VG_(message)(Vg_UserMsg, 
                   "TRACE: %p %s %d thr#%d :: %22s --> %22s",
                   a, how, szB, thr_acc->threadUID, txt_old, txt_new );
   }
}

//
//
// See http://code.google.com/p/data-race-test/wiki/MSMProp1 
// for description. 
//
// This routine is not (yet) fully optimized for performance. 
// TODO: handle state with one segment in segment set separately 
// for better performance. 
static inline
SegmentSet do_SS_update_SINGLE ( /*OUT*/Bool* hb_all_p, 
                                 Thread* thr,
                                 Bool do_trace,
                                 SegmentSet oldSS, SegmentID currS )
{
   // update the segment set and compute hb_all
   /* case where oldSS is a single segment */
   SegmentSet newSS;
   SegmentID S;
   tl_assert(SS_is_singleton(oldSS));
   stats__msm_oldSS_single++;
   S = SS_get_singleton_UNCHECKED(oldSS);
   if (LIKELY(S == currS  // Same segment. 
              || SEG_get(S)->thr == thr // Same thread. 
              || happens_before(S, currS))) {
                 // different thread, but happens-before
      *hb_all_p = True;
      newSS = SS_mk_singleton(currS);
      if (UNLIKELY(0 && do_trace)) {
         VG_(printf)("HB(S%d/T%d,cur)=1\n",
                     S, SEG_get(S)->thr->threadUID);
      }
   } else {
      *hb_all_p = False;
      // Not happened-before. Leave this segment in SS.
      tl_assert(currS != S);
      newSS = HG_(doubletonWS)(univ_ssets, currS, S);
      if (UNLIKELY(0 && do_trace)) {
         VG_(printf)("HB(S%d/T%d,cur)=0\n",
                     S, SEG_get(S)->thr->threadUID);
      }
   }
   return newSS;
}

static __attribute__((noinline))
SegmentSet do_SS_update_MULTI ( /*OUT*/Bool* hb_all_p,
                                Thread* thr,
                                Bool do_trace,
                                SegmentSet oldSS, SegmentID currS )
{
   // update the segment set; compute hb_all and oldSS_has_active_segment. 
   /* General case */

   UWord i;
   UWord oldSS_size = SS_get_size(oldSS);
   SegmentSet newSS = 0;
   SegmentID add_vec[oldSS_size+1]; // C99 array. 
   SegmentID del_vec[oldSS_size+1]; // C99 array. 
   UInt add_size = 0, del_size = 0;

   tl_assert(oldSS_size > 1);
   stats__msm_oldSS_multi++;

   *hb_all_p = True;

   tl_assert(oldSS_size <= clo_max_segment_set_size);

   // fill in the arrays add_vec/del_vec and try a shortcut
   add_vec[add_size++] = currS;
   for (i = 0; i < oldSS_size; i++) {
      SegmentID S = SS_get_element(oldSS, i);
      if (currS == S) {
         // shortcut: 
         // currS is already contained in oldSS, so we don't need to add it. 
         // Since oldSS is a max frontier 
         // (i.e. for each two different segments S1 and S2 from oldSS
         // neither HB(S1,S2) nor HB(S2,S1)) 
         // we don't need to remove anything. 
         // So, return oldSS unchanged. 
         stats__msm_oldSS_multi_shortcut++;
         // none of the segments in SS happend-before currS
         *hb_all_p = False;
         return oldSS;
      }
      // compute happens-before
      Bool hb = False;
      Thread *thr_of_S = SEG_get(S)->thr;

      if (thr_of_S == thr // Same thread. 
          || happens_before(S, currS)) {
             // different thread, but happens-before
         hb = True;
      }
      // trace 
      if (0 && do_trace) {
         VG_(printf)("HB(S%d/T%d,cur)=%d\n",
                     S, SEG_get(S)->thr->threadUID, hb);
      }
      // fill in add_vec or del_vec
      if (!hb) {
         *hb_all_p = False;
         add_vec[add_size++] = S;
      } else {
         del_vec[del_size++] = S;
      }
   }

   // check if we've got a singleton. 
   if (add_size == 1) {
      // add_size == 1 means that currS happend-after all segments in oldSS
      tl_assert(*hb_all_p == True && del_size == oldSS_size);
      return SS_mk_singleton(currS);
   }  
   tl_assert(add_size >= 2);

   // we couldn't have added more than one segment to the set. 
   tl_assert(add_size <= clo_max_segment_set_size+1);
  
   if (add_size == clo_max_segment_set_size + 1) {
      // we've hit the limit of SS size and can't add one more segment. 
      add_size--;
      tl_assert(del_size == 0);
      // we will remove the first segment from the old set 
      // (this segment is likely the oldest one) 
      del_vec[del_size++] = add_vec[1];

      // now del_size==1 and add_size >= 4 (clo_max_segment_set_size >= 4)
      // so we are guaranteed to go to 'del' path below.
   }

   if (add_size - 1 <= del_size + 1) {
      tl_assert(add_size <= clo_max_segment_set_size);
      // create new segment set by adding segments to an empty set. 
      // Requires add_size-1 set operations. 
      for (i = 1; i < add_size; i++) {
         SegmentID S = add_vec[i];
         if (UNLIKELY(i == 1)) {
            newSS = HG_(doubletonWS)(univ_ssets, add_vec[0], S);
         } else {
            newSS = HG_(addToWS)(univ_ssets, newSS, S);
         }
      }
      stats__msm_oldSS_multi_add++;
   } else {
      tl_assert(oldSS_size < clo_max_segment_set_size || del_size > 0);
      // create new segment by removing segments from oldSS
      // and then adding curS. 
      // Requires del_size+1 set operations. 
      newSS = oldSS;
      for (i = 0; i < del_size; i++) {
         newSS = HG_(delFromWS)(univ_ssets, newSS, del_vec[i]);
      }
      newSS = HG_(addToWS)(univ_ssets, newSS, currS);

      tl_assert(SS_get_size(newSS) == add_size);
      stats__msm_oldSS_multi_del++;
   }
   
   // LIKELY (
   //    addToWS & delFromWS operations are equivalent in terms of CPU time
   // )
   
   return newSS;
}

inline
static SegmentSet do_SS_update ( /*OUT*/Bool* hb_all_p, 
                                 Thread* thr,
                                 Bool do_trace,
                                 SegmentSet oldSS, SegmentID currS,
                                 UWord sz)
{  
   SegmentSet newSS;
   if (LIKELY(SS_is_singleton(oldSS))) {
      // we don't care if oldSS contains an active segment since oldSS 
      // is a singleton and we don't want to recycle it. 
      newSS = do_SS_update_SINGLE( hb_all_p, thr, do_trace, oldSS, currS );
      if (UNLIKELY(clo_ss_recycle && !SS_is_singleton(newSS))) {
         // newSS is not singleton => newSS != oldSS. 
         SS_ref(newSS, sz);
         tl_assert(HG_(saneWS_SLOW)(univ_ssets, newSS));
      }
   } else {
      newSS = do_SS_update_MULTI( hb_all_p, thr, do_trace, oldSS, currS );
      if (clo_ss_recycle && newSS != oldSS) {
         if (!SS_is_singleton(newSS)) {
            SS_ref(newSS, sz);
            tl_assert(HG_(saneWS_SLOW)(univ_ssets, newSS));
         }
         SS_unref(oldSS, sz);
      }
   }
   return newSS;
}


// One such object is associated with each traced address.
typedef struct {
   UInt n_accesses;
   // more fields are likely to be added in the future. 
} TraceInfo;


static WordFM *trace_info_map; // addr->TraceInfo;

static TraceInfo *get_trace_info(Addr a)
{
   UWord  key, val;
   TraceInfo *res;
   if (!trace_info_map) {
      trace_info_map = HG_(newFM)( hg_zalloc, hg_free, NULL);
   }

   if (HG_(lookupFM)(trace_info_map, &key, &val, a)) {
      tl_assert(key == (UWord)a);
      return (TraceInfo*)val;
   }
   res = (TraceInfo*)hg_zalloc(sizeof(TraceInfo)); // zero-initialized
   HG_(addToFM)(trace_info_map, (UWord)a, (UWord)res);
   return res;
}

static void msm_do_trace(Thread *thr, 
                         Addr a, 
                         SVal sv_old, 
                         SVal sv_new, 
                         Bool is_w,
                         Int trace_level
                         ) 
{
   HChar buf[200];
   if (sv_old == sv_new) {
      // don't trace if the state is unchanged.
      return; 
   }
   
   TraceInfo *info = get_trace_info(a);
   info->n_accesses++;

   if (info->n_accesses > clo_trace_after_race) {
      // we already printed too many traces
      return;
   }


   show_sval(buf, sizeof(buf), sv_new);
   VG_(message)(Vg_UserMsg, 
                "TRACE[%d] {{{: Access{T%d/S%d %s %p} -> new State{%s}", 
                info->n_accesses,
                (Int)thr->threadUID, 
                (Int)thr->csegid, 
                is_w ? "wr" : "rd", a, buf);
   if (trace_level >= 2) {
      ThreadId tid = map_threads_maybe_reverse_lookup(thr);
      if (tid != VG_INVALID_THREADID) {
         VG_(message)(Vg_UserMsg, " Access stack trace:");
         VG_(get_and_pp_StackTrace)( tid, 15);
      }
   }

   VG_(message)(Vg_UserMsg, " Locks held:");
   show_lockset(is_w ? thr->locksetW : thr->locksetA);
   if (!HG_(isEmptyBag)(&__bus_lock_Lock->heldBy)) {
      VG_(message)(Vg_UserMsg, " BHL is held\n");
   }

   VG_(message)(Vg_UserMsg, "}}}");
   VG_(message)(Vg_UserMsg, ""); // empty line



}

static INLINE 
SVal memory_state_machine(Bool is_w, Thread* thr, Addr a, SVal sv_old, Int sz)
{
   SegmentSet oldSS = 0;
   LockSet    oldLS;
   Bool       hb_all      = False;
   Bool       is_race     = False;
   SVal       sv_new      = SHVAL_Invalid;
   int        trace_level = 
                   (a >= clo_trace_addr  && a < (clo_trace_addr+sz))
                   ? clo_trace_level : 0;
   SegmentID  currS = thr->csegid;
   SegmentSet newSS = 0;
   LockSet    newLS = 0;

   // current locks. 
   LockSet    currLS = is_w ? thr->locksetW : thr->locksetA;
   
   // increment refcount so sv_old isn't deleted between
   //   do_SS_update and record_error_Race calls
   SHVAL_SS_ref(sv_old);

   // Check if trace was requested for this address by a client request.
   if (UNLIKELY(clo_trace_level > 0 && mem_trace_is_on(a))) {
      trace_level = clo_trace_level;
   }

   if (UNLIKELY(clo_ignore_n != 1)) {
      tl_assert(!address_may_be_ignored(a));
   }

   if (UNLIKELY(!is_w && thr->ignore_reads)) {
      sv_new = sv_old;
      goto done;
   }


   if (UNLIKELY(is_SHVAL_Ignore(sv_old))) {
      // we already reported a race, don't bother again. 
      stats__msm_Ignore++;
      sv_new = sv_old;
      goto done;
   }

   if (UNLIKELY(!HG_(isEmptyBag_UNCHECKED)(&__bus_lock_Lock->heldBy)
                && (is_SHVAL_New(sv_old) || is_SHVAL_R(sv_old)))) {
      stats__msm_BHL_hack++;
      // BHL is held and we are in 'Read' or 'New' state. 
      // User is doing something very smart with LOCK prefix.
      // Just ignore this memory location. 
      sv_new = SHVAL_Ignore;

      // VG_(printf)("Ignoring memory %p accessed with LOCK prefix at\n", a);
      // VG_(get_and_pp_StackTrace)(map_threads_reverse_lookup(thr), 5);

      goto done; 
      // TODO: a better scheme might be: 
      // When we see a first write with BHL held we do: 
      // - If we are in state 'Read' or 'New', change the state to 'BHL'. 
      // - If we are in state 'Write', report a race.
      //
      // When we are in state BHL: 
      // - Any read keeps us in state 'BHL'. 
      // - Any write with BHL held keeps us in state 'BHL'. 
      // - Any other write is a race.
   }

   tl_assert(is_sane_Thread(thr));

   // Read or Write
   if (LIKELY(is_SHVAL_RM(sv_old))) {
      Bool was_m, now_m;
      // check the trace bit
      if (UNLIKELY(get_SHVAL_TRACE_BIT(sv_old))) { 
         trace_level = 2;
      }
      
      was_m = is_SHVAL_M(sv_old);

      tl_assert(is_SHVAL_valid_SLOW(sv_old));
      // update the segment set and compute hb_all
      oldSS = get_SHVAL_SS(sv_old);
      newSS = do_SS_update(&hb_all, thr, trace_level >= 1, oldSS, currS, sz);


      // update lock set. 
      if (hb_all) {
         newLS = currLS;
      } else {
         oldLS = get_SHVAL_LS(sv_old);
         newLS = HG_(intersectWS)(univ_lsets, oldLS, currLS);
         if (clo_more_context && oldLS != newLS)
            record_last_lock_lossage( a, oldLS, newLS );
      }

      // update the state 
      now_m = is_w ? True : (was_m && !hb_all);

      // generate new SVal
      sv_new = mk_SHVAL_RM(now_m, newSS, newLS);
      sv_new = set_SHVAL_TRACE_BIT(sv_new, get_SHVAL_TRACE_BIT(sv_old));

      is_race = now_m && !SS_is_singleton(newSS)
                      && HG_(isEmptyWS)(univ_lsets, newLS);

      if      ( (!was_m) && (!now_m) ) stats__msm_R_to_R++;
      else if ( (!was_m) && (now_m) )  stats__msm_R_to_M++;
      else if ( (was_m)  && (!now_m) ) stats__msm_M_to_R++;
      else if ( (was_m)  && (now_m) )  stats__msm_M_to_M++;

      goto done;
   }

   // New
   if (UNLIKELY(is_SHVAL_New(sv_old))) {
      stats__msm_New_to_M++; 
      newSS = SS_mk_singleton(currS);
      sv_new = mk_SHVAL_RM(is_w, newSS, currLS);
      goto done;
   }

   /*NOTREACHED*/
   tl_assert(0);

  done:

   if (UNLIKELY(trace_level >= 1)) {
      msm_do_trace(thr, a, sv_old, sv_new, is_w, trace_level);
   }

   if (clo_trace_level > 0 && trace_level == 0) {
      // if we are tracing something, don't report a race on anything else.
      is_race = False;
   }

   if (UNLIKELY(is_race && get_SHVAL_TRACE_BIT(sv_old))) {
      // Race is found for the second time. 
      // Stop tracing and start ignoring this memory location.
      VG_(message)(Vg_UserMsg, "Race on %p is found again after %u accesses",
                   a, get_trace_info(a)->n_accesses);
      sv_new = SHVAL_Ignore;
      is_race = False;
   }

   // report the race if needed
   if (is_race) {
      // ok, now record the race. 
      Bool race_was_recorded = 
            record_error_Race( thr, 
                         a, is_w, sz, sv_old, sv_new,
                         maybe_get_lastlock_initpoint(a) );
      if (race_was_recorded) {
         // never recycle segment sets in sv_old/sv_new
         SHVAL_SS_ref(sv_old);
         SHVAL_SS_ref(sv_new);
         // if we did record a race and if this mem was not traced before, 
         // turn tracing on.
         sv_new = mk_SHVAL_RM(is_w, SS_mk_singleton(currS), currLS);
         sv_new = set_SHVAL_TRACE_BIT(sv_new, True);
         msm_do_trace(thr, a, sv_old, sv_new, is_w, 2);
      } else {
         // put this in Ignore state
         sv_new = SHVAL_Ignore;
      }
   }
   
   SHVAL_SS_unref(sv_old);

   return sv_new; 
}

static SVal msm_handle_write(Thread* thr, Addr a, SVal sv_old, Int sz)
{
   return memory_state_machine(True, thr, a, sv_old, sz);
}
static SVal msm_handle_read(Thread* thr, Addr a, SVal sv_old, Int sz)
{
   return memory_state_machine(False, thr, a, sv_old, sz);
}


/*----------------------------------------------------------------*/
/*--- Shadow value and address range handlers                  ---*/
/*----------------------------------------------------------------*/

static void laog__pre_thread_acquires_lock ( Thread*, Lock* ); /* fwds */
static void laog__handle_lock_deletions    ( WordSetID ); /* fwds */
static inline Thread* get_current_Thread ( void ); /* fwds */

/* ------------ CacheLineF and CacheLineZ related ------------ */
inline
static void write_twobit_array ( UChar* arr, UWord ix, UWord b2 ) {
   Word bix, shft, mask, prep;
   tl_assert(ix >= 0);
   bix  = ix >> 2;
   shft = 2 * (ix & 3); /* 0, 2, 4 or 6 */
   mask = 3 << shft;
   prep = b2 << shft;
   arr[bix] = (arr[bix] & ~mask) | prep;
}

inline
static UWord read_twobit_array ( UChar* arr, UWord ix ) {
   Word bix, shft;
   tl_assert(ix >= 0);
   bix  = ix >> 2;
   shft = 2 * (ix & 3); /* 0, 2, 4 or 6 */
   return (arr[bix] >> shft) & 3;
}

/* Given a lineZ index and a SecMap, return the CacheLineZ* and CacheLineF*
   for that index. */
static void get_ZF_by_index ( /*OUT*/CacheLineZ** zp,
                              /*OUT*/CacheLineF** fp,
                              SecMap* sm, UInt zix ) {
   CacheLineZ* lineZ;
   tl_assert(zp);
   tl_assert(fp);
   tl_assert(zix >= 0 && zix < N_SECMAP_ZLINES);
   tl_assert(is_sane_SecMap(sm));
   lineZ = &sm->linesZ[zix];
   if (lineZ->dict[0] == 0) {
      UInt fix = (UInt)lineZ->dict[1];
      tl_assert(sm->linesF);
      tl_assert(sm->linesF_size > 0);
      tl_assert(fix >= 0 && fix < sm->linesF_size);
      *zp = NULL;
      *fp = &sm->linesF[fix];
      tl_assert(sm->linesF[fix].inUse);
   } else {
      *zp = lineZ;
      *fp = NULL;
   }
}

static void find_ZF_for_reading ( /*OUT*/CacheLineZ** zp,
                                  /*OUT*/CacheLineF** fp, Addr tag ) {
   CacheLineZ* lineZ;
   CacheLineF* lineF;
   UWord   zix;
   SecMap* sm    = shmem__find_or_alloc_SecMap(tag);
   UWord   smoff = shmem__get_SecMap_offset(tag);
   /* since smoff is derived from a valid tag, it should be
      cacheline-aligned. */
   tl_assert(0 == (smoff & (N_LINE_ARANGE - 1)));
   zix = smoff >> N_LINE_BITS;
   tl_assert(zix < N_SECMAP_ZLINES);
   lineZ = &sm->linesZ[zix];
   lineF = NULL;
   if (lineZ->dict[0] == 0) {
      UInt fix = (UInt)lineZ->dict[1];
      tl_assert(sm->linesF);
      tl_assert(sm->linesF_size > 0);
      tl_assert(fix >= 0 && fix < sm->linesF_size);
      lineF = &sm->linesF[fix];
      tl_assert(lineF->inUse);
      lineZ = NULL;
   }
   *zp = lineZ;
   *fp = lineF;
}

static __attribute__((noinline))
void find_Z_for_writing ( /*OUT*/SecMap** smp,
                          /*OUT*/Word* zixp,
                          Addr tag ) {
   CacheLineZ* lineZ;
   CacheLineF* lineF;
   UWord   zix;
   SecMap* sm    = shmem__find_or_alloc_SecMap(tag);
   UWord   smoff = shmem__get_SecMap_offset(tag);
   /* since smoff is derived from a valid tag, it should be
      cacheline-aligned. */
   tl_assert(0 == (smoff & (N_LINE_ARANGE - 1)));
   zix = smoff >> N_LINE_BITS;
   tl_assert(zix < N_SECMAP_ZLINES);
   lineZ = &sm->linesZ[zix];
   lineF = NULL;
   /* If lineZ has an associated lineF, free it up. */
   if (lineZ->dict[0] == 0) {
      UInt fix = (UInt)lineZ->dict[1];
      tl_assert(sm->linesF);
      tl_assert(sm->linesF_size > 0);
      tl_assert(fix >= 0 && fix < sm->linesF_size);
      lineF = &sm->linesF[fix];
      tl_assert(lineF->inUse);
      lineF->inUse = False;
   }
   *smp  = sm;
   *zixp = zix;
}

static __attribute__((noinline))
void alloc_F_for_writing ( /*MOD*/SecMap* sm, /*OUT*/Word* fixp ) {
   UInt        i, new_size;
   CacheLineF* nyu;

   if (sm->linesF) {
      tl_assert(sm->linesF_size > 0);
   } else {
      tl_assert(sm->linesF_size == 0);
   }

   if (sm->linesF) {
      for (i = 0; i < sm->linesF_size; i++) {
         if (!sm->linesF[i].inUse) {
            *fixp = (Word)i;
            return;
         }
      }
   }

   /* No free F line found.  Expand existing array and try again. */
   new_size = sm->linesF_size==0 ? 1 : 2 * sm->linesF_size;
   nyu      = hg_zalloc( new_size * sizeof(CacheLineF) );
   tl_assert(nyu);

   stats__secmap_linesF_allocd += (new_size - sm->linesF_size);
   stats__secmap_linesF_bytes  += (new_size - sm->linesF_size)
                                  * sizeof(CacheLineF);

   if (0)
   VG_(printf)("SM %p: expand F array from %d to %d\n", 
               sm, (Int)sm->linesF_size, new_size);

   for (i = 0; i < new_size; i++)
      nyu[i].inUse = False;

   if (sm->linesF) {
      for (i = 0; i < sm->linesF_size; i++) {
         tl_assert(sm->linesF[i].inUse);
         nyu[i] = sm->linesF[i];
      }
      VG_(memset)(sm->linesF, 0, sm->linesF_size * sizeof(CacheLineF) );
      hg_free(sm->linesF);
   }

   sm->linesF      = nyu;
   sm->linesF_size = new_size;

   for (i = 0; i < sm->linesF_size; i++) {
      if (!sm->linesF[i].inUse) {
         *fixp = (Word)i;
         return;
      }
    }

    /*NOTREACHED*/
    tl_assert(0);
}


/* ------------ CacheLine and implicit-tree related ------------ */

__attribute__((unused))
static void pp_CacheLine ( CacheLine* cl ) {
   Word i;
   if (!cl) {
      VG_(printf)("pp_CacheLine(NULL)\n");
      return;
   }
   for (i = 0; i < N_LINE_TREES; i++) 
      VG_(printf)("   descr: %04lx\n", (UWord)cl->descrs[i]);
   for (i = 0; i < N_LINE_ARANGE; i++) 
      VG_(printf)("    sval: %08lx\n", (UWord)cl->svals[i]);
}

static UChar descr_to_validbits ( UShort descr )
{
   /* a.k.a Party Time for gcc's constant folder */
#  define DESCR(b8_7, b8_6, b8_5, b8_4, b8_3, b8_2, b8_1, b8_0, \
                b16_3, b32_1, b16_2, b64, b16_1, b32_0, b16_0)  \
             ( (UShort) ( ( (b8_7)  << 14) | ( (b8_6)  << 13) | \
                          ( (b8_5)  << 12) | ( (b8_4)  << 11) | \
                          ( (b8_3)  << 10) | ( (b8_2)  << 9)  | \
                          ( (b8_1)  << 8)  | ( (b8_0)  << 7)  | \
                          ( (b16_3) << 6)  | ( (b32_1) << 5)  | \
                          ( (b16_2) << 4)  | ( (b64)   << 3)  | \
                          ( (b16_1) << 2)  | ( (b32_0) << 1)  | \
                          ( (b16_0) << 0) ) )

#  define BYTE(bit7, bit6, bit5, bit4, bit3, bit2, bit1, bit0) \
             ( (UChar) ( ( (bit7) << 7) | ( (bit6) << 6) | \
                         ( (bit5) << 5) | ( (bit4) << 4) | \
                         ( (bit3) << 3) | ( (bit2) << 2) | \
                         ( (bit1) << 1) | ( (bit0) << 0) ) )

   /* these should all get folded out at compile time */
   tl_assert(DESCR(1,0,0,0,0,0,0,0, 0,0,0, 0, 0,0,0) == TREE_DESCR_8_7);
   tl_assert(DESCR(0,0,0,0,0,0,0,1, 0,0,0, 0, 0,0,0) == TREE_DESCR_8_0);
   tl_assert(DESCR(0,0,0,0,0,0,0,0, 1,0,0, 0, 0,0,0) == TREE_DESCR_16_3);
   tl_assert(DESCR(0,0,0,0,0,0,0,0, 0,1,0, 0, 0,0,0) == TREE_DESCR_32_1);
   tl_assert(DESCR(0,0,0,0,0,0,0,0, 0,0,1, 0, 0,0,0) == TREE_DESCR_16_2);
   tl_assert(DESCR(0,0,0,0,0,0,0,0, 0,0,0, 1, 0,0,0) == TREE_DESCR_64);
   tl_assert(DESCR(0,0,0,0,0,0,0,0, 0,0,0, 0, 1,0,0) == TREE_DESCR_16_1);
   tl_assert(DESCR(0,0,0,0,0,0,0,0, 0,0,0, 0, 0,1,0) == TREE_DESCR_32_0);
   tl_assert(DESCR(0,0,0,0,0,0,0,0, 0,0,0, 0, 0,0,1) == TREE_DESCR_16_0);

   switch (descr) {
   /*
              +--------------------------------- TREE_DESCR_8_7
              |             +------------------- TREE_DESCR_8_0
              |             |  +---------------- TREE_DESCR_16_3
              |             |  | +-------------- TREE_DESCR_32_1
              |             |  | | +------------ TREE_DESCR_16_2
              |             |  | | |  +--------- TREE_DESCR_64
              |             |  | | |  |  +------ TREE_DESCR_16_1
              |             |  | | |  |  | +---- TREE_DESCR_32_0
              |             |  | | |  |  | | +-- TREE_DESCR_16_0
              |             |  | | |  |  | | |
              |             |  | | |  |  | | |   GRANULARITY, 7 -> 0 */
   case DESCR(1,1,1,1,1,1,1,1, 0,0,0, 0, 0,0,0): /* 8 8 8 8  8 8 8 8 */
                                                 return BYTE(1,1,1,1,1,1,1,1);
   case DESCR(1,1,0,0,1,1,1,1, 0,0,1, 0, 0,0,0): /* 8 8 16   8 8 8 8 */
                                                 return BYTE(1,1,0,1,1,1,1,1);
   case DESCR(0,0,1,1,1,1,1,1, 1,0,0, 0, 0,0,0): /* 16  8 8  8 8 8 8 */ 
                                                 return BYTE(0,1,1,1,1,1,1,1);
   case DESCR(0,0,0,0,1,1,1,1, 1,0,1, 0, 0,0,0): /* 16  16   8 8 8 8 */
                                                 return BYTE(0,1,0,1,1,1,1,1);

   case DESCR(1,1,1,1,1,1,0,0, 0,0,0, 0, 0,0,1): /* 8 8 8 8  8 8 16 */ 
                                                 return BYTE(1,1,1,1,1,1,0,1);
   case DESCR(1,1,0,0,1,1,0,0, 0,0,1, 0, 0,0,1): /* 8 8 16   8 8 16 */
                                                 return BYTE(1,1,0,1,1,1,0,1);
   case DESCR(0,0,1,1,1,1,0,0, 1,0,0, 0, 0,0,1): /* 16  8 8  8 8 16 */
                                                 return BYTE(0,1,1,1,1,1,0,1);
   case DESCR(0,0,0,0,1,1,0,0, 1,0,1, 0, 0,0,1): /* 16  16   8 8 16 */
                                                 return BYTE(0,1,0,1,1,1,0,1);

   case DESCR(1,1,1,1,0,0,1,1, 0,0,0, 0, 1,0,0): /* 8 8 8 8  16 8 8 */
                                                 return BYTE(1,1,1,1,0,1,1,1);
   case DESCR(1,1,0,0,0,0,1,1, 0,0,1, 0, 1,0,0): /* 8 8 16   16 8 8 */
                                                 return BYTE(1,1,0,1,0,1,1,1);
   case DESCR(0,0,1,1,0,0,1,1, 1,0,0, 0, 1,0,0): /* 16  8 8  16 8 8 */
                                                 return BYTE(0,1,1,1,0,1,1,1);
   case DESCR(0,0,0,0,0,0,1,1, 1,0,1, 0, 1,0,0): /* 16  16   16 8 8 */
                                                 return BYTE(0,1,0,1,0,1,1,1);

   case DESCR(1,1,1,1,0,0,0,0, 0,0,0, 0, 1,0,1): /* 8 8 8 8  16 16 */
                                                 return BYTE(1,1,1,1,0,1,0,1);
   case DESCR(1,1,0,0,0,0,0,0, 0,0,1, 0, 1,0,1): /* 8 8 16   16 16 */
                                                 return BYTE(1,1,0,1,0,1,0,1);
   case DESCR(0,0,1,1,0,0,0,0, 1,0,0, 0, 1,0,1): /* 16  8 8  16 16 */
                                                 return BYTE(0,1,1,1,0,1,0,1);
   case DESCR(0,0,0,0,0,0,0,0, 1,0,1, 0, 1,0,1): /* 16  16   16 16 */
                                                 return BYTE(0,1,0,1,0,1,0,1);

   case DESCR(0,0,0,0,1,1,1,1, 0,1,0, 0, 0,0,0): /* 32  8 8 8 8 */
                                                 return BYTE(0,0,0,1,1,1,1,1);
   case DESCR(0,0,0,0,1,1,0,0, 0,1,0, 0, 0,0,1): /* 32  8 8 16  */
                                                 return BYTE(0,0,0,1,1,1,0,1);
   case DESCR(0,0,0,0,0,0,1,1, 0,1,0, 0, 1,0,0): /* 32  16  8 8 */
                                                 return BYTE(0,0,0,1,0,1,1,1);
   case DESCR(0,0,0,0,0,0,0,0, 0,1,0, 0, 1,0,1): /* 32  16  16  */
                                                 return BYTE(0,0,0,1,0,1,0,1);

   case DESCR(1,1,1,1,0,0,0,0, 0,0,0, 0, 0,1,0): /* 8 8 8 8  32 */
                                                 return BYTE(1,1,1,1,0,0,0,1);
   case DESCR(1,1,0,0,0,0,0,0, 0,0,1, 0, 0,1,0): /* 8 8 16   32 */
                                                 return BYTE(1,1,0,1,0,0,0,1);
   case DESCR(0,0,1,1,0,0,0,0, 1,0,0, 0, 0,1,0): /* 16  8 8  32 */
                                                 return BYTE(0,1,1,1,0,0,0,1);
   case DESCR(0,0,0,0,0,0,0,0, 1,0,1, 0, 0,1,0): /* 16  16   32 */
                                                 return BYTE(0,1,0,1,0,0,0,1);

   case DESCR(0,0,0,0,0,0,0,0, 0,1,0, 0, 0,1,0): /* 32 32 */
                                                 return BYTE(0,0,0,1,0,0,0,1);

   case DESCR(0,0,0,0,0,0,0,0, 0,0,0, 1, 0,0,0): /* 64 */
                                                 return BYTE(0,0,0,0,0,0,0,1);

   default: return BYTE(0,0,0,0,0,0,0,0); 
                   /* INVALID - any valid descr produces at least one
                      valid bit in tree[0..7]*/
   }
   /* NOTREACHED*/
   tl_assert(0);

#  undef DESCR
#  undef BYTE
}

__attribute__((unused))
static Bool is_sane_Descr ( UShort descr ) {
   return descr_to_validbits(descr) != 0;
}

static void sprintf_Descr ( /*OUT*/UChar* dst, UShort descr ) {
   VG_(sprintf)(dst, 
                "%d%d%d%d%d%d%d%d %d%d%d %d %d%d%d",
                (Int)((descr & TREE_DESCR_8_7) ? 1 : 0),
                (Int)((descr & TREE_DESCR_8_6) ? 1 : 0),
                (Int)((descr & TREE_DESCR_8_5) ? 1 : 0),
                (Int)((descr & TREE_DESCR_8_4) ? 1 : 0),
                (Int)((descr & TREE_DESCR_8_3) ? 1 : 0),
                (Int)((descr & TREE_DESCR_8_2) ? 1 : 0),
                (Int)((descr & TREE_DESCR_8_1) ? 1 : 0),
                (Int)((descr & TREE_DESCR_8_0) ? 1 : 0),
                (Int)((descr & TREE_DESCR_16_3) ? 1 : 0),
                (Int)((descr & TREE_DESCR_32_1) ? 1 : 0),
                (Int)((descr & TREE_DESCR_16_2) ? 1 : 0),
                (Int)((descr & TREE_DESCR_64)   ? 1 : 0),
                (Int)((descr & TREE_DESCR_16_1) ? 1 : 0),
                (Int)((descr & TREE_DESCR_32_0) ? 1 : 0),
                (Int)((descr & TREE_DESCR_16_0) ? 1 : 0)
   );
}
static void sprintf_Byte ( /*OUT*/UChar* dst, UChar byte ) {
   VG_(sprintf)(dst, "%d%d%d%d%d%d%d%d",
                     (Int)((byte & 128) ? 1 : 0),
                     (Int)((byte &  64) ? 1 : 0),
                     (Int)((byte &  32) ? 1 : 0),
                     (Int)((byte &  16) ? 1 : 0),
                     (Int)((byte &   8) ? 1 : 0),
                     (Int)((byte &   4) ? 1 : 0),
                     (Int)((byte &   2) ? 1 : 0),
                     (Int)((byte &   1) ? 1 : 0)
   );
}

static Bool is_sane_Descr_and_Tree ( UShort descr, SVal* tree ) {
   Word  i;
   UChar validbits = descr_to_validbits(descr);
   UChar buf[128], buf2[128];
   if (validbits == 0)
      goto bad;
   for (i = 0; i < 8; i++) {
      if (validbits & (1<<i)) {
         if (!is_SHVAL_valid(tree[i]))
            goto bad;
      } else {
         if (tree[i] != 0)
            goto bad;
      }
   }
   return True;
  bad:
   sprintf_Descr( buf, descr );
   sprintf_Byte( buf2, validbits );
   VG_(printf)("is_sane_Descr_and_Tree: bad tree {\n");
   VG_(printf)("   validbits 0x%02lx    %s\n", (UWord)validbits, buf2);
   VG_(printf)("       descr 0x%04lx  %s\n", (UWord)descr, buf);
   for (i = 0; i < 8; i++)
      VG_(printf)("   [%ld] 0x%08x\n", i, tree[i]);
   VG_(printf)("}\n");
   return 0;
}

static Bool is_sane_CacheLine ( CacheLine* cl )
{
   Word tno, cloff;

   if (!cl) goto bad;

   for (tno = 0, cloff = 0;  tno < N_LINE_TREES;  tno++, cloff += 8) {
      UShort descr = cl->descrs[tno];
      SVal*  tree  = &cl->svals[cloff];
      if (!is_sane_Descr_and_Tree(descr, tree))
         goto bad;
   }
   tl_assert(cloff == N_LINE_ARANGE);
   return True;
  bad:
   pp_CacheLine(cl);
   return False;
}

static UShort normalise_tree ( /*MOD*/SVal* tree )
{
   UShort descr;
   /* pre: incoming tree[0..7] does not have any invalid shvals, in
      particular no zeroes. */
   if (UNLIKELY(tree[7] == 0 || tree[6] == 0 || tree[5] == 0
                || tree[4] == 0 || tree[3] == 0 || tree[2] == 0
                || tree[1] == 0 || tree[0] == 0))
      tl_assert(0);
   
   descr = TREE_DESCR_8_7 | TREE_DESCR_8_6 | TREE_DESCR_8_5
           | TREE_DESCR_8_4 | TREE_DESCR_8_3 | TREE_DESCR_8_2
           | TREE_DESCR_8_1 | TREE_DESCR_8_0;
   /* build 16-bit layer */
   if (tree[1] == tree[0]) {
      tree[1] = 0/*INVALID*/;
      descr &= ~(TREE_DESCR_8_1 | TREE_DESCR_8_0);
      descr |= TREE_DESCR_16_0;
   }
   if (tree[3] == tree[2]) {
      tree[3] = 0/*INVALID*/;
      descr &= ~(TREE_DESCR_8_3 | TREE_DESCR_8_2);
      descr |= TREE_DESCR_16_1;
   }
   if (tree[5] == tree[4]) {
      tree[5] = 0/*INVALID*/;
      descr &= ~(TREE_DESCR_8_5 | TREE_DESCR_8_4);
      descr |= TREE_DESCR_16_2;
   }
   if (tree[7] == tree[6]) {
      tree[7] = 0/*INVALID*/;
      descr &= ~(TREE_DESCR_8_7 | TREE_DESCR_8_6);
      descr |= TREE_DESCR_16_3;
   }
   /* build 32-bit layer */
   if (tree[2] == tree[0]
       && (descr & TREE_DESCR_16_1) && (descr & TREE_DESCR_16_0)) {
      tree[2] = 0; /* [3,1] must already be 0 */
      descr &= ~(TREE_DESCR_16_1 | TREE_DESCR_16_0);
      descr |= TREE_DESCR_32_0;
   }
   if (tree[6] == tree[4]
       && (descr & TREE_DESCR_16_3) && (descr & TREE_DESCR_16_2)) {
      tree[6] = 0; /* [7,5] must already be 0 */
      descr &= ~(TREE_DESCR_16_3 | TREE_DESCR_16_2);
      descr |= TREE_DESCR_32_1;
   }
   /* build 64-bit layer */
   if (tree[4] == tree[0]
       && (descr & TREE_DESCR_32_1) && (descr & TREE_DESCR_32_0)) {
      tree[4] = 0; /* [7,6,5,3,2,1] must already be 0 */
      descr &= ~(TREE_DESCR_32_1 | TREE_DESCR_32_0);
      descr |= TREE_DESCR_64;
   }
   return descr;
}

/* This takes a cacheline where all the data is at the leaves
   (w8[..]) and builds a correctly normalised tree. */
static void normalise_CacheLine ( /*MOD*/CacheLine* cl )
{
   Word tno, cloff;
   for (tno = 0, cloff = 0;  tno < N_LINE_TREES;  tno++, cloff += 8) {
      SVal* tree = &cl->svals[cloff];
      cl->descrs[tno] = normalise_tree( tree );
   }
   tl_assert(cloff == N_LINE_ARANGE);
   if (SCE_CACHELINE)
      tl_assert(is_sane_CacheLine(cl)); /* EXPENSIVE */
   stats__cline_normalises++;
}


typedef struct { UChar count; SVal sval; } CountedSVal;

static
void sequentialise_CacheLine ( /*OUT*/CountedSVal* dst,
                               /*OUT*/Word* dstUsedP,
                               Word nDst, CacheLine* src )
{
   Word  tno, cloff, dstUsed;

   tl_assert(nDst == N_LINE_ARANGE);
   dstUsed = 0;

   for (tno = 0, cloff = 0;  tno < N_LINE_TREES;  tno++, cloff += 8) {
      UShort descr = src->descrs[tno];
      SVal*  tree  = &src->svals[cloff];

      /* sequentialise the tree described by (descr,tree). */
#     define PUT(_n,_v)                                \
         do { dst[dstUsed  ].count = (_n);             \
              dst[dstUsed++].sval  = (_v);             \
         } while (0)

      /* byte 0 */
      if (descr & TREE_DESCR_64)   PUT(8, tree[0]); else
      if (descr & TREE_DESCR_32_0) PUT(4, tree[0]); else
      if (descr & TREE_DESCR_16_0) PUT(2, tree[0]); else
      if (descr & TREE_DESCR_8_0)  PUT(1, tree[0]);
      /* byte 1 */
      if (descr & TREE_DESCR_8_1)  PUT(1, tree[1]);
      /* byte 2 */
      if (descr & TREE_DESCR_16_1) PUT(2, tree[2]); else
      if (descr & TREE_DESCR_8_2)  PUT(1, tree[2]);
      /* byte 3 */
      if (descr & TREE_DESCR_8_3)  PUT(1, tree[3]);
      /* byte 4 */
      if (descr & TREE_DESCR_32_1) PUT(4, tree[4]); else
      if (descr & TREE_DESCR_16_2) PUT(2, tree[4]); else
      if (descr & TREE_DESCR_8_4)  PUT(1, tree[4]);
      /* byte 5 */
      if (descr & TREE_DESCR_8_5)  PUT(1, tree[5]);
      /* byte 6 */
      if (descr & TREE_DESCR_16_3) PUT(2, tree[6]); else
      if (descr & TREE_DESCR_8_6)  PUT(1, tree[6]);
      /* byte 7 */
      if (descr & TREE_DESCR_8_7)  PUT(1, tree[7]);

#     undef PUT
      /* END sequentialise the tree described by (descr,tree). */

   }
   tl_assert(cloff == N_LINE_ARANGE);
   tl_assert(dstUsed <= nDst);

   *dstUsedP = dstUsed;
}

/* Write the cacheline 'wix' to backing store.  Where it ends up
   is determined by its tag field. */
static __attribute__((noinline)) void cacheline_wback ( UWord wix )
{
   Word        i, j, k, m;
   Addr        tag;
   SecMap*     sm;
   CacheLine*  cl;
   CacheLineZ* lineZ;
   CacheLineF* lineF;
   Word        zix, fix, csvalsUsed;
   CountedSVal csvals[N_LINE_ARANGE];
   SVal        sv;

   if (0)
   VG_(printf)("scache wback line %d\n", (Int)wix);

   tl_assert(wix >= 0 && wix < N_WAY_NENT);

   tag =  cache_shmem.tags0[wix];
   cl  = &cache_shmem.lyns0[wix];

   /* The cache line may have been invalidated; if so, ignore it. */
   if (!is_valid_scache_tag(tag))
      return;

   /* Where are we going to put it? */
   sm         = NULL;
   lineZ      = NULL;
   lineF      = NULL;
   zix = fix = -1;

   find_Z_for_writing( &sm, &zix, tag );
   tl_assert(sm);
   tl_assert(zix >= 0 && zix < N_SECMAP_ZLINES);
   lineZ = &sm->linesZ[zix];

   /* Generate the data to be stored */
   if (SCE_CACHELINE)
      tl_assert(is_sane_CacheLine(cl)); /* EXPENSIVE */

   csvalsUsed = -1;
   sequentialise_CacheLine( csvals, &csvalsUsed, 
                            N_LINE_ARANGE, cl );
   tl_assert(csvalsUsed >= 1 && csvalsUsed <= N_LINE_ARANGE);
   if (0) VG_(printf)("%lu ", csvalsUsed);

   lineZ->dict[0] = lineZ->dict[1] 
                  = lineZ->dict[2] = lineZ->dict[3] = 0;

   /* i indexes actual shadow values, k is cursor in csvals */
   i = 0;
   for (k = 0; k < csvalsUsed; k++) {

      sv = csvals[k].sval;
      if (SCE_SVALS)
         tl_assert(csvals[k].count >= 1 && csvals[k].count <= 8);
      /* do we already have it? */
      if (sv == lineZ->dict[0]) { j = 0; goto dict_ok; }
      if (sv == lineZ->dict[1]) { j = 1; goto dict_ok; }
      if (sv == lineZ->dict[2]) { j = 2; goto dict_ok; }
      if (sv == lineZ->dict[3]) { j = 3; goto dict_ok; }
      /* no.  look for a free slot. */
      if (SCE_SVALS)
         tl_assert(is_SHVAL_valid(sv));
      if (lineZ->dict[0] == 0) { lineZ->dict[0] = sv; j = 0; goto dict_ok; }
      if (lineZ->dict[1] == 0) { lineZ->dict[1] = sv; j = 1; goto dict_ok; }
      if (lineZ->dict[2] == 0) { lineZ->dict[2] = sv; j = 2; goto dict_ok; }
      if (lineZ->dict[3] == 0) { lineZ->dict[3] = sv; j = 3; goto dict_ok; }
      break; /* we'll have to use the f rep */
     dict_ok:
      m = csvals[k].count;
      if (m == 8) {
         write_twobit_array( lineZ->ix2s, i+0, j );
         write_twobit_array( lineZ->ix2s, i+1, j );
         write_twobit_array( lineZ->ix2s, i+2, j );
         write_twobit_array( lineZ->ix2s, i+3, j );
         write_twobit_array( lineZ->ix2s, i+4, j );
         write_twobit_array( lineZ->ix2s, i+5, j );
         write_twobit_array( lineZ->ix2s, i+6, j );
         write_twobit_array( lineZ->ix2s, i+7, j );
         i += 8;
      }
      else if (m == 4) {
         write_twobit_array( lineZ->ix2s, i+0, j );
         write_twobit_array( lineZ->ix2s, i+1, j );
         write_twobit_array( lineZ->ix2s, i+2, j );
         write_twobit_array( lineZ->ix2s, i+3, j );
         i += 4;
      }
      else if (m == 1) {
         write_twobit_array( lineZ->ix2s, i+0, j );
         i += 1;
      }
      else if (m == 2) {
         write_twobit_array( lineZ->ix2s, i+0, j );
         write_twobit_array( lineZ->ix2s, i+1, j );
         i += 2;
      }
      else {
         tl_assert(0); /* 8 4 2 or 1 are the only legitimate values for m */
      }

   }

   if (LIKELY(i == N_LINE_ARANGE)) {
      /* Construction of the compressed representation was
         successful. */
      stats__cache_Z_wbacks++;
   } else {
      /* Cannot use the compressed(z) representation.  Use the full(f)
         rep instead. */

      tl_assert(i >= 0 && i < N_LINE_ARANGE);
      alloc_F_for_writing( sm, &fix );
      tl_assert(sm->linesF);
      tl_assert(sm->linesF_size > 0);
      tl_assert(fix >= 0 && fix < (Word)sm->linesF_size);
      lineF = &sm->linesF[fix];
      tl_assert(!lineF->inUse);
      lineZ->dict[0] = lineZ->dict[2] = lineZ->dict[3] = 0;
      lineZ->dict[1] = (SVal)fix;
      lineF->inUse = True;
      i = 0;
      for (k = 0; k < csvalsUsed; k++) {
         if (SCE_SVALS)
            tl_assert(csvals[k].count >= 1 && csvals[k].count <= 8);
         sv = csvals[k].sval;
         if (SCE_SVALS)
            tl_assert(is_SHVAL_valid(sv));
         for (m = csvals[k].count; m > 0; m--) {
            lineF->w64s[i] = sv;
            i++;
         }
      }
      tl_assert(i == N_LINE_ARANGE);
      stats__cache_F_wbacks++;
   }

   //if (anyShared)
   //   sm->mbHasShared = True;

   /* mb_tidy_one_cacheline(); */
}

/* Fetch the cacheline 'wix' from the backing store.  The tag
   associated with 'wix' is assumed to have already been filled in;
   hence that is used to determine where in the backing store to read
   from. */
static __attribute__((noinline)) void cacheline_fetch ( UWord wix )
{
   Word        i;
   Addr        tag;
   CacheLine*  cl;
   CacheLineZ* lineZ;
   CacheLineF* lineF;

   if (0)
   VG_(printf)("scache fetch line %d\n", (Int)wix);

   tl_assert(wix >= 0 && wix < N_WAY_NENT);

   tag =  cache_shmem.tags0[wix];
   cl  = &cache_shmem.lyns0[wix];

   /* reject nonsense requests */
   tl_assert(is_valid_scache_tag(tag));

   lineZ = NULL;
   lineF = NULL;
   find_ZF_for_reading( &lineZ, &lineF, tag );
   tl_assert( (lineZ && !lineF) || (!lineZ && lineF) );

   /* expand the data into the bottom layer of the tree, then get
      cacheline_normalise to build the descriptor array. */
   if (lineF) {
      tl_assert(lineF->inUse);
      // TODO: performance - memcpy?
      for (i = 0; i < N_LINE_ARANGE; i++) {
         cl->svals[i] = lineF->w64s[i];
      }
      stats__cache_F_fetches++;
   } else {
      for (i = 0; i < N_LINE_ARANGE; i++) {
         SVal sv;
         UWord ix = read_twobit_array( lineZ->ix2s, i );
         /* correct, but expensive: tl_assert(ix >= 0 && ix <= 3); */
         sv = lineZ->dict[ix];
         tl_assert(sv != 0);
         cl->svals[i] = sv;
      }
      stats__cache_Z_fetches++;
   }
   normalise_CacheLine( cl );
}

static void shmem__invalidate_scache ( void ) {
   Word wix;
   if (0) VG_(printf)("scache inval\n");
   tl_assert(!is_valid_scache_tag(1));
   for (wix = 0; wix < N_WAY_NENT; wix++) {
      cache_shmem.tags0[wix] = 1/*INVALID*/;
   }
   stats__cache_invals++;
}

static void shmem__flush_and_invalidate_scache ( void ) {
   Word wix;
   Addr tag;
   if (0) VG_(printf)("scache flush and invalidate\n");
   tl_assert(!is_valid_scache_tag(1));
   for (wix = 0; wix < N_WAY_NENT; wix++) {
      tag = cache_shmem.tags0[wix];
      if (tag == 1/*INVALID*/) {
         /* already invalid; nothing to do */
      } else {
         tl_assert(is_valid_scache_tag(tag));
         cacheline_wback( wix );
      }
      cache_shmem.tags0[wix] = 1/*INVALID*/;
   }
   stats__cache_flushes++;
   stats__cache_invals++;
}


/* ------------ Basic shadow memory read/write ops ------------ */

// handle clo_ignore_n and clo_ignore_i.
static inline Bool address_may_be_ignored ( Addr a ) {
   UWord w = (UWord)a;
   UWord n = clo_ignore_n;
   UWord i = clo_ignore_i;
   const Int sh = N_SECMAP_BITS;
   tl_assert(n != 1); // must not be called if clo_ignore_n == 1
   if (n == 0) return True;
   // Optimize for the case when clo_ignore_n is a power of two.
   if ((n & (n-1)) == 0) return (((w >> sh) & (n-1)) != i);
   // Optimize for some more values. 
   if (n == 3)  return (((w >> sh) % 3) != i);
   if (n == 7)  return (((w >> sh) % 7) != i);
   if (n == 13) return (((w >> sh) % 13) != i);
   // general case: slow (division). 
   if (((w >> sh) % n) != i) return True;
   return False;
}


static inline Bool aligned16 ( Addr a ) {
   return 0 == (a & 1);
}
static inline Bool aligned32 ( Addr a ) {
   return 0 == (a & 3);
}
static inline Bool aligned64 ( Addr a ) {
   return 0 == (a & 7);
}
static inline UWord get_cacheline_offset ( Addr a ) {
   return (UWord)(a & (N_LINE_ARANGE - 1));
}
static inline Addr cacheline_ROUNDUP ( Addr a ) {
   return VG_ROUNDUP(a, N_LINE_ARANGE);
}
static inline Addr cacheline_ROUNDDN ( Addr a ) {
   return VG_ROUNDDN(a, N_LINE_ARANGE);
}
static inline UWord get_treeno ( Addr a ) {
   return get_cacheline_offset(a) >> 3;
}
static inline UWord get_tree_offset ( Addr a ) {
   return a & 7;
}

static __attribute__((noinline))
       CacheLine* get_cacheline_MISS ( Addr a ); /* fwds */
static inline CacheLine* get_cacheline ( Addr a )
{
   /* tag is 'a' with the in-line offset masked out, 
      eg a[31]..a[4] 0000 */
   Addr       tag = a & ~(N_LINE_ARANGE - 1);
   UWord      wix = (a >> N_LINE_BITS) & (N_WAY_NENT - 1);
   stats__cache_totrefs++;
   if (LIKELY(tag == cache_shmem.tags0[wix])) {
      return &cache_shmem.lyns0[wix];
   } else {
      return get_cacheline_MISS( a );
   }
}

static __attribute__((noinline))
       CacheLine* get_cacheline_MISS ( Addr a )
{
   /* tag is 'a' with the in-line offset masked out, 
      eg a[31]..a[4] 0000 */

   CacheLine* cl;
   Addr*      tag_old_p;
   Addr       tag = a & ~(N_LINE_ARANGE - 1);
   UWord      wix = (a >> N_LINE_BITS) & (N_WAY_NENT - 1);

   tl_assert(tag != cache_shmem.tags0[wix]);

   /* Dump the old line into the backing store. */
   stats__cache_totmisses++;

   cl        = &cache_shmem.lyns0[wix];
   tag_old_p = &cache_shmem.tags0[wix];

   if (is_valid_scache_tag( *tag_old_p )) {
      /* EXPENSIVE and REDUNDANT: callee does it */
      if (SCE_CACHELINE)
         tl_assert(is_sane_CacheLine(cl)); /* EXPENSIVE */
      cacheline_wback( wix );
   }
   /* and reload the new one */
   *tag_old_p = tag;
   cacheline_fetch( wix );
   if (SCE_CACHELINE)
      tl_assert(is_sane_CacheLine(cl)); /* EXPENSIVE */
   return cl;
}

static UShort pulldown_to_32 ( /*MOD*/SVal* tree, UWord toff, UShort descr ) {
   stats__cline_64to32pulldown++;
   switch (toff) {
      case 0: case 4:
         tl_assert(descr & TREE_DESCR_64);
         tree[4] = tree[0];
         descr &= ~TREE_DESCR_64;
         descr |= (TREE_DESCR_32_1 | TREE_DESCR_32_0);
         break;
      default:
         tl_assert(0);
   }
   return descr;
}

static UShort pulldown_to_16 ( /*MOD*/SVal* tree, UWord toff, UShort descr ) {
   stats__cline_32to16pulldown++;
   switch (toff) {
      case 0: case 2:
         if (!(descr & TREE_DESCR_32_0)) {
            descr = pulldown_to_32(tree, 0, descr);
         }
         tl_assert(descr & TREE_DESCR_32_0);
         tree[2] = tree[0];
         descr &= ~TREE_DESCR_32_0;
         descr |= (TREE_DESCR_16_1 | TREE_DESCR_16_0);
         break;
      case 4: case 6:
         if (!(descr & TREE_DESCR_32_1)) {
            descr = pulldown_to_32(tree, 4, descr);
         }
         tl_assert(descr & TREE_DESCR_32_1);
         tree[6] = tree[4];
         descr &= ~TREE_DESCR_32_1;
         descr |= (TREE_DESCR_16_3 | TREE_DESCR_16_2);
         break;
      default:
         tl_assert(0);
   }
   return descr;
}

static UShort pulldown_to_8 ( /*MOD*/SVal* tree, UWord toff, UShort descr ) {
   stats__cline_16to8pulldown++;
   switch (toff) {
      case 0: case 1:
         if (!(descr & TREE_DESCR_16_0)) {
            descr = pulldown_to_16(tree, 0, descr);
         }
         tl_assert(descr & TREE_DESCR_16_0);
         tree[1] = tree[0];
         descr &= ~TREE_DESCR_16_0;
         descr |= (TREE_DESCR_8_1 | TREE_DESCR_8_0);
         break;
      case 2: case 3:
         if (!(descr & TREE_DESCR_16_1)) {
            descr = pulldown_to_16(tree, 2, descr);
         }
         tl_assert(descr & TREE_DESCR_16_1);
         tree[3] = tree[2];
         descr &= ~TREE_DESCR_16_1;
         descr |= (TREE_DESCR_8_3 | TREE_DESCR_8_2);
         break;
      case 4: case 5:
         if (!(descr & TREE_DESCR_16_2)) {
            descr = pulldown_to_16(tree, 4, descr);
         }
         tl_assert(descr & TREE_DESCR_16_2);
         tree[5] = tree[4];
         descr &= ~TREE_DESCR_16_2;
         descr |= (TREE_DESCR_8_5 | TREE_DESCR_8_4);
         break;
      case 6: case 7:
         if (!(descr & TREE_DESCR_16_3)) {
            descr = pulldown_to_16(tree, 6, descr);
         }
         tl_assert(descr & TREE_DESCR_16_3);
         tree[7] = tree[6];
         descr &= ~TREE_DESCR_16_3;
         descr |= (TREE_DESCR_8_7 | TREE_DESCR_8_6);
         break;
      default:
         tl_assert(0);
   }
   return descr;
}


static UShort pullup_descr_to_16 ( UShort descr, UWord toff ) {
   UShort mask;
   switch (toff) {
      case 0:
         mask = TREE_DESCR_8_1 | TREE_DESCR_8_0;
         tl_assert( (descr & mask) == mask );
         descr &= ~mask;
         descr |= TREE_DESCR_16_0;
         break;
      case 2:
         mask = TREE_DESCR_8_3 | TREE_DESCR_8_2;
         tl_assert( (descr & mask) == mask );
         descr &= ~mask;
         descr |= TREE_DESCR_16_1;
         break;
      case 4:
         mask = TREE_DESCR_8_5 | TREE_DESCR_8_4;
         tl_assert( (descr & mask) == mask );
         descr &= ~mask;
         descr |= TREE_DESCR_16_2;
         break;
      case 6:
         mask = TREE_DESCR_8_7 | TREE_DESCR_8_6;
         tl_assert( (descr & mask) == mask );
         descr &= ~mask;
         descr |= TREE_DESCR_16_3;
         break;
      default:
         tl_assert(0);
   }
   return descr;
}

static UShort pullup_descr_to_32 ( UShort descr, UWord toff ) {
   UShort mask;
   switch (toff) {
      case 0:
         if (!(descr & TREE_DESCR_16_0))
            descr = pullup_descr_to_16(descr, 0);
         if (!(descr & TREE_DESCR_16_1))
            descr = pullup_descr_to_16(descr, 2);
         mask = TREE_DESCR_16_1 | TREE_DESCR_16_0;
         tl_assert( (descr & mask) == mask );
         descr &= ~mask;
         descr |= TREE_DESCR_32_0;
         break;
      case 4:
         if (!(descr & TREE_DESCR_16_2))
            descr = pullup_descr_to_16(descr, 4);
         if (!(descr & TREE_DESCR_16_3))
            descr = pullup_descr_to_16(descr, 6);
         mask = TREE_DESCR_16_3 | TREE_DESCR_16_2;
         tl_assert( (descr & mask) == mask );
         descr &= ~mask;
         descr |= TREE_DESCR_32_1;
         break;
      default:
         tl_assert(0);
   }
   return descr;
}

static Bool valid_value_is_above_me_32 ( UShort descr, UWord toff ) {
   switch (toff) {
      case 0: case 4:
         return 0 != (descr & TREE_DESCR_64);
      default:
         tl_assert(0);
   }
}

static Bool valid_value_is_below_me_16 ( UShort descr, UWord toff ) {
   switch (toff) {
      case 0:
         return 0 != (descr & (TREE_DESCR_8_1 | TREE_DESCR_8_0));
      case 2:
         return 0 != (descr & (TREE_DESCR_8_3 | TREE_DESCR_8_2));
      case 4:
         return 0 != (descr & (TREE_DESCR_8_5 | TREE_DESCR_8_4));
      case 6:
         return 0 != (descr & (TREE_DESCR_8_7 | TREE_DESCR_8_6));
      default:
         tl_assert(0);
   }
}

static void shadow_mem_read8 ( Thread* thr_acc, Addr a, SVal uuOpaque ) {
   CacheLine* cl; 
   UWord      cloff, tno, toff;
   SVal       svOld, svNew;
   UShort     descr;
   if (UNLIKELY(clo_ignore_n != 1 && address_may_be_ignored(a))) return;
   stats__cline_read8s++;
   cl    = get_cacheline(a);
   cloff = get_cacheline_offset(a);
   tno   = get_treeno(a);
   toff  = get_tree_offset(a); /* == 0 .. 7 */
   descr = cl->descrs[tno];
   if (UNLIKELY( !(descr & (TREE_DESCR_8_0 << toff)) )) {
      SVal* tree = &cl->svals[tno << 3];
      cl->descrs[tno] = pulldown_to_8(tree, toff, descr);
      if (SCE_CACHELINE)
         tl_assert(is_sane_CacheLine(cl)); /* EXPENSIVE */
   }
   svOld = cl->svals[cloff];
   svNew = msm_handle_read( thr_acc, a, svOld, 1 );
   cl->svals[cloff] = svNew;
}
static void shadow_mem_read16 ( Thread* thr_acc, Addr a, SVal uuOpaque ) {
   CacheLine* cl; 
   UWord      cloff, tno, toff;
   SVal       svOld, svNew;
   UShort     descr;
   if (UNLIKELY(clo_ignore_n != 1 && address_may_be_ignored(a))) return;
   stats__cline_read16s++;
   if (UNLIKELY(!aligned16(a))) goto slowcase;
   cl    = get_cacheline(a);
   cloff = get_cacheline_offset(a);
   // TODO: (tno can work faster if it knows cloff) ?
   tno   = get_treeno(a);
   toff  = get_tree_offset(a); /* == 0, 2, 4 or 6 */
   descr = cl->descrs[tno];
   if (UNLIKELY( !(descr & (TREE_DESCR_16_0 << toff)) )) {
      if (valid_value_is_below_me_16(descr, toff)) {
         goto slowcase;
      } else {
         SVal* tree = &cl->svals[tno << 3];
         cl->descrs[tno] = pulldown_to_16(tree, toff, descr);
      }
      if (SCE_CACHELINE)
         tl_assert(is_sane_CacheLine(cl)); /* EXPENSIVE */
   }
   svOld = cl->svals[cloff];
   svNew = msm_handle_read( thr_acc, a, svOld, 2 );
   cl->svals[cloff] = svNew;
   return;
  slowcase: /* misaligned, or must go further down the tree */
   stats__cline_16to8splits++;
   shadow_mem_read8( thr_acc, a + 0, 0/*unused*/ );
   shadow_mem_read8( thr_acc, a + 1, 0/*unused*/ );
}

__attribute__((noinline))
static void shadow_mem_read32_SLOW ( Thread* thr_acc, Addr a, SVal uuOpaque ) {
   CacheLine* cl; 
   UWord      cloff, tno, toff;
   SVal       svOld, svNew;
   UShort     descr;
   if (UNLIKELY(clo_ignore_n != 1 && address_may_be_ignored(a))) return;
   if (UNLIKELY(!aligned32(a))) goto slowcase;
   cl    = get_cacheline(a);
   cloff = get_cacheline_offset(a);
   tno   = get_treeno(a);
   toff  = get_tree_offset(a); /* == 0 or 4 */
   descr = cl->descrs[tno];
   if (UNLIKELY( !(descr & (TREE_DESCR_32_0 << toff)) )) {
      if (valid_value_is_above_me_32(descr, toff)) {
         SVal* tree = &cl->svals[tno << 3];
         cl->descrs[tno] = pulldown_to_32(tree, toff, descr);
      } else {
         goto slowcase;
      }
      if (SCE_CACHELINE)
         tl_assert(is_sane_CacheLine(cl)); /* EXPENSIVE */
   }
   svOld = cl->svals[cloff];
   svNew = msm_handle_read( thr_acc, a, svOld, 4 );
   cl->svals[cloff] = svNew;
   return;
  slowcase: /* misaligned, or must go further down the tree */
   stats__cline_32to16splits++;
   shadow_mem_read16( thr_acc, a + 0, 0/*unused*/ );
   shadow_mem_read16( thr_acc, a + 2, 0/*unused*/ );
}
inline
static void shadow_mem_read32 ( Thread* thr_acc, Addr a, SVal uuOpaque ) {
   CacheLine* cl; 
   UWord      cloff, tno, toff;
   UShort     descr;
   if (UNLIKELY(clo_ignore_n != 1 && address_may_be_ignored(a))) return;
   stats__cline_read32s++;
   if (UNLIKELY(!aligned32(a))) goto slowcase;
   cl    = get_cacheline(a);
   cloff = get_cacheline_offset(a);
   tno   = get_treeno(a);
   toff  = get_tree_offset(a); /* == 0 or 4 */
   descr = cl->descrs[tno];
   if (UNLIKELY( !(descr & (TREE_DESCR_32_0 << toff)) )) goto slowcase;
   { SVal* p = &cl->svals[cloff];
     *p = msm_handle_read( thr_acc, a, *p, 4 );
   }
   return;
  slowcase: /* misaligned, or not at this level in the tree */
   shadow_mem_read32_SLOW( thr_acc, a, uuOpaque );
}

inline
static void shadow_mem_read64 ( Thread* thr_acc, Addr a, SVal uuOpaque ) {
   CacheLine* cl; 
   UWord      cloff, tno, toff;
   SVal       svOld, svNew;
   UShort     descr;
   if (UNLIKELY(clo_ignore_n != 1 && address_may_be_ignored(a))) return;
   stats__cline_read64s++;
   if (UNLIKELY(!aligned64(a))) goto slowcase;
   cl    = get_cacheline(a);
   cloff = get_cacheline_offset(a);
   tno   = get_treeno(a);
   toff  = get_tree_offset(a); /* == 0, unused */
   descr = cl->descrs[tno];
   if (UNLIKELY( !(descr & TREE_DESCR_64) )) {
      goto slowcase;
   }
   svOld = cl->svals[cloff];
   svNew = msm_handle_read( thr_acc, a, svOld, 8 );
   cl->svals[cloff] = svNew;
   return;
  slowcase: /* misaligned, or must go further down the tree */
   stats__cline_64to32splits++;
   shadow_mem_read32( thr_acc, a + 0, 0/*unused*/ );
   shadow_mem_read32( thr_acc, a + 4, 0/*unused*/ );
}

static void shadow_mem_write8 ( Thread* thr_acc, Addr a, SVal uuOpaque ) {
   CacheLine* cl; 
   UWord      cloff, tno, toff;
   SVal       svOld, svNew;
   UShort     descr;
   if (UNLIKELY(clo_ignore_n != 1 && address_may_be_ignored(a))) return;
   stats__cline_write8s++;
   cl    = get_cacheline(a);
   cloff = get_cacheline_offset(a);
   tno   = get_treeno(a);
   toff  = get_tree_offset(a); /* == 0 .. 7 */
   descr = cl->descrs[tno];
   if (UNLIKELY( !(descr & (TREE_DESCR_8_0 << toff)) )) {
      SVal* tree = &cl->svals[tno << 3];
      cl->descrs[tno] = pulldown_to_8(tree, toff, descr);
      if (SCE_CACHELINE)
         tl_assert(is_sane_CacheLine(cl)); /* EXPENSIVE */
   }
   svOld = cl->svals[cloff];
   svNew = msm_handle_write( thr_acc, a, svOld, 1 );
   cl->svals[cloff] = svNew;
}
static void shadow_mem_write16 ( Thread* thr_acc, Addr a, SVal uuOpaque ) {
   CacheLine* cl; 
   UWord      cloff, tno, toff;
   SVal       svOld, svNew;
   UShort     descr;
   if (UNLIKELY(clo_ignore_n != 1 && address_may_be_ignored(a))) return;
   stats__cline_write16s++;
   if (UNLIKELY(!aligned16(a))) goto slowcase;
   cl    = get_cacheline(a);
   cloff = get_cacheline_offset(a);
   tno   = get_treeno(a);
   toff  = get_tree_offset(a); /* == 0, 2, 4 or 6 */
   descr = cl->descrs[tno];
   if (UNLIKELY( !(descr & (TREE_DESCR_16_0 << toff)) )) {
      if (valid_value_is_below_me_16(descr, toff)) {
         goto slowcase;
      } else {
         SVal* tree = &cl->svals[tno << 3];
         cl->descrs[tno] = pulldown_to_16(tree, toff, descr);
      }
      if (SCE_CACHELINE)
         tl_assert(is_sane_CacheLine(cl)); /* EXPENSIVE */
   }
   svOld = cl->svals[cloff];
   svNew = msm_handle_write( thr_acc, a, svOld, 2 );
   cl->svals[cloff] = svNew;
   return;
  slowcase: /* misaligned, or must go further down the tree */
   stats__cline_16to8splits++;
   shadow_mem_write8( thr_acc, a + 0, 0/*unused*/ );
   shadow_mem_write8( thr_acc, a + 1, 0/*unused*/ );
}

__attribute__((noinline))
static void shadow_mem_write32_SLOW ( Thread* thr_acc, Addr a, SVal uuOpaque ) {
   CacheLine* cl; 
   UWord      cloff, tno, toff;
   SVal       svOld, svNew;
   UShort     descr;
   if (UNLIKELY(clo_ignore_n != 1 && address_may_be_ignored(a))) return;
   if (UNLIKELY(!aligned32(a))) goto slowcase;
   cl    = get_cacheline(a);
   cloff = get_cacheline_offset(a);
   tno   = get_treeno(a);
   toff  = get_tree_offset(a); /* == 0 or 4 */
   descr = cl->descrs[tno];
   if (UNLIKELY( !(descr & (TREE_DESCR_32_0 << toff)) )) {
      if (valid_value_is_above_me_32(descr, toff)) {
         SVal* tree = &cl->svals[tno << 3];
         cl->descrs[tno] = pulldown_to_32(tree, toff, descr);
      } else {
         goto slowcase;
      }
      if (SCE_CACHELINE)
         tl_assert(is_sane_CacheLine(cl)); /* EXPENSIVE */
   }
   svOld = cl->svals[cloff];
   svNew = msm_handle_write( thr_acc, a, svOld, 4 );
   cl->svals[cloff] = svNew;
   return;
  slowcase: /* misaligned, or must go further down the tree */
   stats__cline_32to16splits++;
   shadow_mem_write16( thr_acc, a + 0, 0/*unused*/ );
   shadow_mem_write16( thr_acc, a + 2, 0/*unused*/ );
}
inline
static void shadow_mem_write32 ( Thread* thr_acc, Addr a, SVal uuOpaque ) {
   CacheLine* cl; 
   UWord      cloff, tno, toff;
   UShort     descr;
   if (UNLIKELY(clo_ignore_n != 1 && address_may_be_ignored(a))) return;
   stats__cline_write32s++;
   if (UNLIKELY(!aligned32(a))) goto slowcase;
   cl    = get_cacheline(a);
   cloff = get_cacheline_offset(a);
   tno   = get_treeno(a);
   toff  = get_tree_offset(a); /* == 0 or 4 */
   descr = cl->descrs[tno];
   if (UNLIKELY( !(descr & (TREE_DESCR_32_0 << toff)) )) goto slowcase;
   { SVal* p = &cl->svals[cloff];
     *p = msm_handle_write( thr_acc, a, *p, 4 );
   }
   return;
  slowcase: /* misaligned, or must go further down the tree */
   shadow_mem_write32_SLOW( thr_acc, a, uuOpaque );
}

inline
static void shadow_mem_write64 ( Thread* thr_acc, Addr a, SVal uuOpaque ) {
   CacheLine* cl; 
   UWord      cloff, tno, toff;
   SVal       svOld, svNew;
   UShort     descr;
   if (UNLIKELY(clo_ignore_n != 1 && address_may_be_ignored(a))) return;
   stats__cline_write64s++;
   if (UNLIKELY(!aligned64(a))) goto slowcase;
   cl    = get_cacheline(a);
   cloff = get_cacheline_offset(a);
   tno   = get_treeno(a);
   toff  = get_tree_offset(a); /* == 0, unused */
   descr = cl->descrs[tno];
   if (UNLIKELY( !(descr & TREE_DESCR_64) )) {
      goto slowcase;
   }
   svOld = cl->svals[cloff];
   svNew = msm_handle_write( thr_acc, a, svOld, 8 );
   cl->svals[cloff] = svNew;
   return;
  slowcase: /* misaligned, or must go further down the tree */
   stats__cline_64to32splits++;
   shadow_mem_write32( thr_acc, a + 0, 0/*unused*/ );
   shadow_mem_write32( thr_acc, a + 4, 0/*unused*/ );
}

static void shadow_mem_set8 ( Thread* uu_thr_acc, Addr a, SVal svNew ) {
   CacheLine* cl; 
   UWord      cloff, tno, toff;
   UShort     descr;
   if (UNLIKELY(clo_ignore_n != 1 && address_may_be_ignored(a))) return;
   stats__cline_set8s++;
   cl    = get_cacheline(a);
   cloff = get_cacheline_offset(a);
   tno   = get_treeno(a);
   toff  = get_tree_offset(a); /* == 0 .. 7 */
   descr = cl->descrs[tno];
   if (UNLIKELY( !(descr & (TREE_DESCR_8_0 << toff)) )) {
      SVal* tree = &cl->svals[tno << 3];
      cl->descrs[tno] = pulldown_to_8(tree, toff, descr);
      if (SCE_CACHELINE)
         tl_assert(is_sane_CacheLine(cl)); /* EXPENSIVE */
   }
   cl->svals[cloff] = svNew;
}
static void shadow_mem_set16 ( Thread* uu_thr_acc, Addr a, SVal svNew ) {
   CacheLine* cl; 
   UWord      cloff, tno, toff;
   UShort     descr;
   if (UNLIKELY(clo_ignore_n != 1 && address_may_be_ignored(a))) return;
   stats__cline_set16s++;
   if (UNLIKELY(!aligned16(a))) goto slowcase;
   cl    = get_cacheline(a);
   cloff = get_cacheline_offset(a);
   tno   = get_treeno(a);
   toff  = get_tree_offset(a); /* == 0, 2, 4 or 6 */
   descr = cl->descrs[tno];
   if (UNLIKELY( !(descr & (TREE_DESCR_16_0 << toff)) )) {
      if (valid_value_is_below_me_16(descr, toff)) {
         /* Writing at this level.  Need to fix up 'descr'. */
         cl->descrs[tno] = pullup_descr_to_16(descr, toff);
         /* At this point, the tree does not match cl->descr[tno] any
            more.  The assignments below will fix it up. */
      } else {
         /* We can't indiscriminately write on the w16 node as in the
            w64 case, as that might make the node inconsistent with
            its parent.  So first, pull down to this level. */
         SVal* tree = &cl->svals[tno << 3];
         cl->descrs[tno] = pulldown_to_16(tree, toff, descr);
      if (SCE_CACHELINE)
         tl_assert(is_sane_CacheLine(cl)); /* EXPENSIVE */
      }
   }
   cl->svals[cloff + 0] = svNew;
   cl->svals[cloff + 1] = 0;
   return;
  slowcase: /* misaligned */
   stats__cline_16to8splits++;
   shadow_mem_set8( uu_thr_acc, a + 0, svNew );
   shadow_mem_set8( uu_thr_acc, a + 1, svNew );
}
static void shadow_mem_set32 ( Thread* uu_thr_acc, Addr a, SVal svNew ) {
   CacheLine* cl; 
   UWord      cloff, tno, toff;
   UShort     descr;
   if (UNLIKELY(clo_ignore_n != 1 && address_may_be_ignored(a))) return;
   stats__cline_set32s++;
   if (UNLIKELY(!aligned32(a))) goto slowcase;
   cl    = get_cacheline(a);
   cloff = get_cacheline_offset(a);
   tno   = get_treeno(a);
   toff  = get_tree_offset(a); /* == 0 or 4 */
   descr = cl->descrs[tno];
   if (UNLIKELY( !(descr & (TREE_DESCR_32_0 << toff)) )) {
      if (valid_value_is_above_me_32(descr, toff)) {
         /* We can't indiscriminately write on the w32 node as in the
            w64 case, as that might make the node inconsistent with
            its parent.  So first, pull down to this level. */
         SVal* tree = &cl->svals[tno << 3];
         cl->descrs[tno] = pulldown_to_32(tree, toff, descr);
         if (SCE_CACHELINE)
            tl_assert(is_sane_CacheLine(cl)); /* EXPENSIVE */
      } else {
         /* Writing at this level.  Need to fix up 'descr'. */
         cl->descrs[tno] = pullup_descr_to_32(descr, toff);
         /* At this point, the tree does not match cl->descr[tno] any
            more.  The assignments below will fix it up. */
      }
   }
   cl->svals[cloff + 0] = svNew;
   cl->svals[cloff + 1] = 0;
   cl->svals[cloff + 2] = 0;
   cl->svals[cloff + 3] = 0;
   return;
  slowcase: /* misaligned */
   stats__cline_32to16splits++;
   shadow_mem_set16( uu_thr_acc, a + 0, svNew );
   shadow_mem_set16( uu_thr_acc, a + 2, svNew );
}
inline
static void shadow_mem_set64 ( Thread* uu_thr_acc, Addr a, SVal svNew ) {
   CacheLine* cl; 
   UWord      cloff, tno, toff;
   if (UNLIKELY(clo_ignore_n != 1 && address_may_be_ignored(a))) return;
   stats__cline_set64s++;
   if (UNLIKELY(!aligned64(a))) goto slowcase;
   cl    = get_cacheline(a);
   cloff = get_cacheline_offset(a);
   tno   = get_treeno(a);
   toff  = get_tree_offset(a); /* == 0 */
   cl->descrs[tno] = TREE_DESCR_64;
   cl->svals[cloff + 0] = svNew;
   cl->svals[cloff + 1] = 0;
   cl->svals[cloff + 2] = 0;
   cl->svals[cloff + 3] = 0;
   cl->svals[cloff + 4] = 0;
   cl->svals[cloff + 5] = 0;
   cl->svals[cloff + 6] = 0;
   cl->svals[cloff + 7] = 0;
   return;
  slowcase: /* misaligned */
   stats__cline_64to32splits++;
   shadow_mem_set32( uu_thr_acc, a + 0, svNew );
   shadow_mem_set32( uu_thr_acc, a + 4, svNew );
}

static SVal shadow_mem_get8 ( Addr a ) {
   CacheLine* cl; 
   UWord      cloff, tno, toff;
   UShort     descr;
   stats__cline_get8s++;
   cl    = get_cacheline(a);
   cloff = get_cacheline_offset(a);
   tno   = get_treeno(a);
   toff  = get_tree_offset(a); /* == 0 .. 7 */
   descr = cl->descrs[tno];
   if (UNLIKELY( !(descr & (TREE_DESCR_8_0 << toff)) )) {
      SVal* tree = &cl->svals[tno << 3];
      cl->descrs[tno] = pulldown_to_8(tree, toff, descr);
   }
   return cl->svals[cloff];
}

static void shadow_mem_copy8 ( Addr src, Addr dst, Bool normalise ) {
   SVal       sv;
   stats__cline_copy8s++;
   sv = shadow_mem_get8( src );

   if (UNLIKELY(clo_trace_level > 0)) {
      if (dst == clo_trace_addr) {
         Thread* thr    = get_current_Thread();
         SVal    sv_old = shadow_mem_get8( dst );
         msm__show_state_change( thr, dst, 1, 'w', sv_old, sv );
      }
   }

   shadow_mem_set8( NULL/*unused*/, dst, sv );
}


/* ------------ Shadow memory range setting ops ------------ */

static void shadow_mem_modify_range(
               Thread* thr, 
               Addr    a, 
               SizeT   len,
               void    (*fn8) (Thread*,Addr,SVal),
               void    (*fn16)(Thread*,Addr,SVal),
               void    (*fn32)(Thread*,Addr,SVal),
               void    (*fn64)(Thread*,Addr,SVal),
               SVal    opaque
            )
{
   /* fast track a couple of common cases */
   if (len == 4 && aligned32(a)) {
      fn32( thr, a, opaque );
      return;
   }
   if (len == 8 && aligned64(a)) {
      fn64( thr, a, opaque );
      return;
   }

   /* be completely general (but as efficient as possible) */
   if (len == 0) return;

   if (!aligned16(a) && len >= 1) {
      fn8( thr, a, opaque );
      a += 1;
      len -= 1;
      tl_assert(aligned16(a));
   }
   if (len == 0) return;

   if (!aligned32(a) && len >= 2) {
      fn16( thr, a, opaque );
      a += 2;
      len -= 2;
      tl_assert(aligned32(a));
   }
   if (len == 0) return;

   if (!aligned64(a) && len >= 4) {
      fn32( thr, a, opaque );
      a += 4;
      len -= 4;
      tl_assert(aligned64(a));
   }
   if (len == 0) return;

   if (len >= 8) {
      tl_assert(aligned64(a));
      while (len >= 8) {
         fn64( thr, a, opaque );
         a += 8;
         len -= 8;
      }
      tl_assert(aligned64(a));
   }
   if (len == 0) return;

   if (len >= 4)
      tl_assert(aligned32(a));
   if (len >= 4) {
      fn32( thr, a, opaque );
      a += 4;
      len -= 4;
   }
   if (len == 0) return;

   if (len >= 2)
      tl_assert(aligned16(a));
   if (len >= 2) {
      fn16( thr, a, opaque );
      a += 2;
      len -= 2;
   }
   if (len == 0) return;

   if (len >= 1) {
      fn8( thr, a, opaque );
      a += 1;
      len -= 1;
   }
   tl_assert(len == 0);
}

/* Block-copy states (needed for implementing realloc()). */
static void shadow_mem_copy_range ( Addr src, Addr dst, SizeT len )
{
   SizeT i;
   if (len == 0)
      return;
   /* To be simple, just copy byte by byte.  But so as not to wreck
      performance for later accesses to dst[0 .. len-1], normalise
      destination lines as we finish with them, and also normalise the
      line containing the first and last address. */
   for (i = 0; i < len; i++) {
      Bool normalise
         = get_cacheline_offset( dst+i+1 ) == 0 /* last in line */
           || i == 0       /* first in range */
           || i == len-1;  /* last in range */
      shadow_mem_copy8( src+i, dst+i, normalise );
   }
}

static void shadow_mem_read_range ( Thread* thr, Addr a, SizeT len ) {
   shadow_mem_modify_range( thr, a, len, 
                            shadow_mem_read8,
                            shadow_mem_read16,
                            shadow_mem_read32,
                            shadow_mem_read64,
                            0/*opaque,ignored*/ );
}

static void shadow_mem_write_range ( Thread* thr, Addr a, SizeT len ) {
   shadow_mem_modify_range( thr, a, len, 
                            shadow_mem_write8,
                            shadow_mem_write16,
                            shadow_mem_write32,
                            shadow_mem_write64,
                            0/*opaque,ignored*/ );
}

static void shadow_mem_make_New ( Thread* thr, Addr a, SizeT len )
{
   stats__cache_make_New_arange += (ULong)len;

   if (0 && len > 500)
      VG_(printf)("make New      ( %p, %ld )\n", a, len );

   if (UNLIKELY(clo_trace_level > 0)) {
      if (len > 0 && a <= clo_trace_addr && clo_trace_addr < a+len) {
         SVal sv_old = shadow_mem_get8( clo_trace_addr );
         msm__show_state_change( thr, a, (Int)len, 'p', sv_old, SHVAL_New );
      }
   }

   if (0) {
     static UWord n_New_in_cache = 0;
     static UWord n_New_not_in_cache = 0;
     /* tag is 'a' with the in-line offset masked out, 
        eg a[31]..a[4] 0000 */
     Addr       tag = a & ~(N_LINE_ARANGE - 1);
     UWord      wix = (a >> N_LINE_BITS) & (N_WAY_NENT - 1);
     if (LIKELY(tag == cache_shmem.tags0[wix])) {
        n_New_in_cache++;
     } else {
        n_New_not_in_cache++;
     }
     if (0 == ((n_New_in_cache + n_New_not_in_cache) % 100000))
        VG_(printf)("shadow_mem_make_New: IN %lu OUT %lu\n",
                    n_New_in_cache, n_New_not_in_cache );
   }

   if (LIKELY(len < 2 * N_LINE_ARANGE)) {
      shadow_mem_modify_range( thr, a, len, 
                               shadow_mem_set8,
                               shadow_mem_set16,
                               shadow_mem_set32,
                               shadow_mem_set64,
                               SHVAL_New/*opaque*/ );
   } else {
      Addr  before_start  = a;
      Addr  aligned_start = cacheline_ROUNDUP(a);
      Addr  after_start   = cacheline_ROUNDDN(a + len);
      UWord before_len    = aligned_start - before_start;
      UWord aligned_len   = after_start - aligned_start;
      UWord after_len     = a + len - after_start;
      tl_assert(before_start <= aligned_start);
      tl_assert(aligned_start <= after_start);
      tl_assert(before_len < N_LINE_ARANGE);
      tl_assert(after_len < N_LINE_ARANGE);
      tl_assert(get_cacheline_offset(aligned_start) == 0);
      if (get_cacheline_offset(a) == 0) {
         tl_assert(before_len == 0);
         tl_assert(a == aligned_start);
      }
      if (get_cacheline_offset(a+len) == 0) {
         tl_assert(after_len == 0);
         tl_assert(after_start == a+len);
      }
      if (before_len > 0) {
         shadow_mem_modify_range( thr, before_start, before_len, 
                                  shadow_mem_set8,
                                  shadow_mem_set16,
                                  shadow_mem_set32,
                                  shadow_mem_set64,
                                  SHVAL_New/*opaque*/ );
      }
      if (after_len > 0) {
         shadow_mem_modify_range( thr, after_start, after_len, 
                                  shadow_mem_set8,
                                  shadow_mem_set16,
                                  shadow_mem_set32,
                                  shadow_mem_set64,
                                  SHVAL_New/*opaque*/ );
      }
      stats__cache_make_New_inZrep += (ULong)aligned_len;

      while (1) {
         if (aligned_start >= after_start)
            break;
         tl_assert(get_cacheline_offset(aligned_start) == 0);
         Addr  tag = aligned_start & ~(N_LINE_ARANGE - 1);
         UWord wix = (aligned_start >> N_LINE_BITS) & (N_WAY_NENT - 1);
         if (tag == cache_shmem.tags0[wix]) {
            UWord i;
            for (i = 0; i < N_LINE_ARANGE / 8; i++)
               shadow_mem_set64( thr, aligned_start + i * 8, SHVAL_New );
         } else {
            UWord i;
            Word zix;
            SecMap* sm;
            CacheLineZ* lineZ;
            /* This line is not in the cache.  Do not force it in; instead
               modify it in-place. */
            find_Z_for_writing( &sm, &zix, tag );
            tl_assert(sm);
            tl_assert(zix >= 0 && zix < N_SECMAP_ZLINES);
            lineZ = &sm->linesZ[zix];
            lineZ->dict[0] = SHVAL_New;
            lineZ->dict[1] = lineZ->dict[2] = lineZ->dict[3] = 0;
            for (i = 0; i < N_LINE_ARANGE/4; i++)
               lineZ->ix2s[i] = 0; /* all refer to dict[0] */
         }
         aligned_start += N_LINE_ARANGE;
         aligned_len -= N_LINE_ARANGE;
      }
      tl_assert(aligned_start == after_start);
      tl_assert(aligned_len == 0);
   }
}


/* Putting memory into the NoAccess state.  This is hugely complicated
   by the problem of memory that contains locks.

   1. Examine the .mbHasLocks fields in all SecMaps in the range to be
      deleted.  This quickly indicates if there are or might be any
      locks in the range to be deleted.  Note that .mbHasLocks fields on
      SecMaps are not subject to scaching, so it safe to look at them
      without flushing the scache.

   2. Set the range to NoAccess.  Clear the .mbHasShared and
      .mbHasLocks hint bits for any completely vacated SecMaps.
      Clearing the hint bits isn't necessary for correctness, but it
      is important to avoid ending up with hint bits being permanently
      set, which would render them pointless.

   3. If (1) indicated "definitely no locks", we're done.  This is
      the fast and hopefully common case.

   Otherwise, the range contains some locks (or may do), so we have to
   go to considerable effort to tidy up.

   4. Make up a set containing the locks which are deleted:

      ToDelete = NULL

      for each lk in map_locks {
         if lk's guest addr falls in the range to memory be deleted
            add lk to ToDelete

         if lk is held, issue an error message - freeing memory
            containing a held lock
      }

   5. If ToDelete is empty, there were in fact no locks in the range,
      despite what the .mbHasLocks hint bits indicated.  We're done.

   6. Flush the scache.  This is necessary both to bring the SecMap
      .mbHasShared fields up to date, and to bring the actual shadow
      values up to date.  We will need to examine both of these.

      Invalidate the scache.  This is necessary because we will be
      modifying values in the backing store (SecMaps) and need
      subsequent shmem accesses to get the new values.

      KCC: I removed step 7 from the code. Do we really need it? 

   7. Modify all shadow words, by removing ToDelete from the lockset
      of all ShM and ShR states.  Note this involves a complete scan
      over map_shmem, which is very expensive according to OProfile.
      Hence it depends critically on the size of each entry in
      map_shmem.  See comments on definition of N_SECMAP_BITS above.

      Why is it safe to do (7) after (2) ?  Because we're not
      interested in messing with ShR/M states which are going to be
      set to NoAccess anyway.

      Optimisation 1 (implemented): skip this step for SecMaps which
      do not have .mbHasShared set

      Optimisation 2 (not implemented): for each SecMap, have a
      summary lock set which is the union of all locks mentioned in
      locksets on this page (or any superset of it).  Then skip step
      (2) if the summary lockset does not intersect with ToDelete.

      That's potentially cheap, since the usual lockset refinement
      only shrinks locksets; hence there is no point in updating the
      summary lockset for ShM/R -> ShM/R transitions.  Therefore only
      need to do this for Excl->ShM/R transitions.

   8. Tell laog that these locks have disappeared.
*/
static void shadow_mem_make_NoAccess ( Thread* thr, Addr aIN, SizeT len )
{
   Lock*     lk;
   Addr      gla, sma, firstSM, lastSM, firstA, lastA;
   WordSetID locksToDelete;
   Bool      mbHasLocks;

   if (0 && len > 500)
      VG_(printf)("make NoAccess ( %p, %d )\n", aIN, len );

   if (len == 0) 
      return;

   /* --- Step 1 --- */

   firstA  = aIN;
   lastA   = aIN + len - 1;

   firstSM = shmem__round_to_SecMap_base( firstA );
   lastSM  = shmem__round_to_SecMap_base( lastA );
   tl_assert(firstSM <= lastSM);

   mbHasLocks = False;
   for (sma = firstSM; sma <= lastSM; sma += N_SECMAP_ARANGE) {
      if (shmem__get_mbHasLocks(sma)) {
         mbHasLocks = True;
         break;
      }
   }

   // turn off memory trace
   mem_trace_off(firstA, lastA);

   /* --- Step 2 --- */

   // Get rid of this (perhaps)
   if (UNLIKELY(clo_trace_level > 0)) {
      if (len > 0 && firstA <= clo_trace_addr && clo_trace_addr <= lastA) {
         SVal sv_old = shadow_mem_get8( clo_trace_addr );
         msm__show_state_change( thr, firstA, (Int)len, 'p',
                                      sv_old, SHVAL_New );
      }
   }

   for (sma = firstSM; sma <= lastSM; sma += N_SECMAP_ARANGE) {
      /* Is this sm entirely within the deleted range? */
      if (firstA <= sma && sma + N_SECMAP_ARANGE - 1 <= lastA) {
         /* Yes.  Clear the hint bits. */
         shmem__set_mbHasLocks( sma, False );
         //shmem__set_mbHasShared( sma, False );
      }
   }

   /* --- Step 3 --- */

   if (!mbHasLocks)
      return;

   /* --- Step 4 --- */

   if (0) 
   VG_(printf)("shadow_mem_make_NoAccess(%p, %u, %p): maybe slow case\n",
               (void*)firstA, (UWord)len, (void*)lastA);
   locksToDelete = HG_(emptyWS)( univ_lsets );
   
   /* Iterate over all locks in the range firstA .. lastA inclusive. */
   HG_(initIterAtFM)( map_locks, firstA );
   while (HG_(nextIterFM)( map_locks, (Word*)&gla, (Word*)&lk )
          && gla <= lastA) {
      tl_assert(is_sane_LockN(lk));
      tl_assert(gla >= firstA);
      tl_assert(gla <= lastA);

      locksToDelete = HG_(addToWS)( univ_lsets, locksToDelete, (Word)lk );
      /* If the lock is held, we must remove it from the currlock sets
         of all threads that hold it.  Also take the opportunity to
         report an error.  To report an error we need to know at least
         one of the threads that holds it; really we should mention
         them all, but that's too much hassle.  So choose one
         arbitrarily. */
      if (!HG_(isEmptyBag)(&lk->heldBy)) {
         record_error_FreeMemLock( 
            (Thread*)HG_(anyElementOfBag)(&lk->heldBy),
            lk
         );
         /* remove lock from locksets of all owning threads */
         remove_Lock_from_locksets_of_all_owning_Threads( lk );
      }
   }
   HG_(doneIterFM)( map_locks );

   /* --- Step 5 --- */

   if (HG_(isEmptyWS)( univ_lsets, locksToDelete ))
      return;

   /* --- Step 6 --- */

   if (0) shmem__flush_and_invalidate_scache();

   /* --- Step 7 --- */

   if (0) 
   VG_(printf)("shadow_mem_make_NoAccess(%p, %u, %p): definitely slow case\n",
               (void*)firstA, (UWord)len, (void*)lastA);

   /* Now we have to free up the Locks in locksToDelete and remove
      any mention of them from admin_locks and map_locks.  This is
      inefficient. */
   { Lock* lkprev = NULL;
     lk = admin_locks;
     while (True) {
        if (lk == NULL) break;
        if (lkprev) tl_assert(lkprev->admin == lk);

        if (!HG_(elemWS)(univ_lsets, locksToDelete, (Word)lk)) {
           lkprev = lk;
           lk = lk->admin;
           continue;
        }
        /* Need to delete 'lk' */
        if (lkprev == NULL) {
           admin_locks = lk->admin;
        } else {
           lkprev->admin = lk->admin;
        }
        /* and get it out of map_locks */
        map_locks_delete(lk->guestaddr);
        unset_mu_is_cv(lk->guestaddr);
        /* release storage (incl. associated .heldBy Bag) */
        /* XXX NO: we must let locks live forever now.  Consider this:
           client frees memory containing a lock.  However, a SVal
           could reference a LockSet which references this Lock.  If
           we free the Lock then we have a dangling pointer.  Since
           scanning all shadow memory so as to remove this Lock from
           all LockSets is unfeasibly expensive, it's simpler just to
           let the lock live forever. */
        { Lock* tmp = lk->admin;
          //del_LockN(lk);
          lk = tmp;
        }
     }
   }

   /* --- Step 8 --- */

   /* update lock order acquisition graph */
   laog__handle_lock_deletions( locksToDelete );

   if (0) all__sanity_check("Make NoAccess");
}


/*----------------------------------------------------------------*/
/*--- Event handlers (evh__* functions)                        ---*/
/*--- plus helpers (evhH__* functions)                         ---*/
/*----------------------------------------------------------------*/

/*--------- Event handler helpers (evhH__* functions) ---------*/


static void evhH__do_cv_signal(Thread *thr, Word cond);
static Bool evhH__do_cv_wait(Thread *thr, Word cond, Bool must_match_signal);

/* Create a new segment for 'thr', making it depend (.prev) on its
   existing segment, bind together the SegmentID and Segment, and
   return both of them.  Also update 'thr' so it references the new
   Segment. */
static 
void evhH__start_new_segment_for_thread ( /*OUT*/SegmentID* new_segidP,
                                          /*OUT*/Segment** new_segP,
                                          Thread* thr )
{
   Segment* cur_seg;
   tl_assert(new_segP);
   tl_assert(new_segidP);
   tl_assert(is_sane_Thread(thr));
   cur_seg = SEG_get( thr->csegid );
   tl_assert(cur_seg);
   tl_assert(cur_seg->thr == thr); /* all sane segs should point back
                                      at their owner thread. */
   *new_segidP = mk_Segment( thr, cur_seg, NULL/*other*/ );
   *new_segP   = SEG_get(*new_segidP);
   thr->csegid = *new_segidP;

   ThreadId tid = map_threads_maybe_reverse_lookup(thr);
   if (clo_more_context && tid != VG_INVALID_THREADID) {
      SEG_set_context(*new_segidP,
                      VG_(record_ExeContext)(tid,-1/*first_ip_delta*/));
#if 0
      if (*new_segidP % 50 == 0) {
        VG_(printf)("Segment sample (SegmentID = %d) {\n", (int)*new_segidP);
        VG_(pp_ExeContext)((*new_segP)->context);
        VG_(printf)("}\n");
      }
#endif
   }
}


/* The lock at 'lock_ga' has acquired a writer.  Make all necessary
   updates, and also do all possible error checks. */
static 
void evhH__post_thread_w_acquires_lock ( Thread* thr, 
                                         LockKind lkk, Addr lock_ga )
{
   Lock* lk; 

   /* Basically what we need to do is call lockN_acquire_writer.
      However, that will barf if any 'invalid' lock states would
      result.  Therefore check before calling.  Side effect is that
      'is_sane_LockN(lk)' is both a pre- and post-condition of this
      routine. 

      Because this routine is only called after successful lock
      acquisition, we should not be asked to move the lock into any
      invalid states.  Requests to do so are bugs in libpthread, since
      that should have rejected any such requests. */

   /* be paranoid w.r.t hint bits, even if lock_ga is complete
      nonsense */
   shmem__set_mbHasLocks( lock_ga, True );

   tl_assert(is_sane_Thread(thr));
   /* Try to find the lock.  If we can't, then create a new one with
      kind 'lkk'. */
   lk = map_locks_lookup_or_create( 
           lkk, lock_ga, map_threads_reverse_lookup(thr) );
   tl_assert( is_sane_LockN(lk) );
   shmem__set_mbHasLocks( lock_ga, True );

   if (HG_(isEmptyBag)(&lk->heldBy)) {
      /* the lock isn't held.  Simple. */
      tl_assert(!lk->heldW);
      lockN_acquire_writer( lk, thr );
      goto noerror;
   }

   /* So the lock is already held.  If held as a r-lock then
      libpthread must be buggy. */
   if (!lk->heldW) {
      record_error_Misc( thr, "Bug in libpthread: write lock "
                              "granted on rwlock which is currently rd-held");
      goto error;
   }

   /* So the lock is held in w-mode.  If it's held by some other
      thread, then libpthread must be buggy. */
   tl_assert(HG_(sizeUniqueBag)(&lk->heldBy) == 1); /* from precondition */

   if (thr != (Thread*)HG_(anyElementOfBag)(&lk->heldBy)) {
      record_error_Misc( thr, "Bug in libpthread: write lock "
                              "granted on mutex/rwlock which is currently "
                              "wr-held by a different thread");
      goto error;
   }

   /* So the lock is already held in w-mode by 'thr'.  That means this
      is an attempt to lock it recursively, which is only allowable
      for LK_mbRec kinded locks.  Since this routine is called only
      once the lock has been acquired, this must also be a libpthread
      bug. */
   if (lk->kind != LK_mbRec) {
      record_error_Misc( thr, "Bug in libpthread: recursive write lock "
                              "granted on mutex/wrlock which does not "
                              "support recursion");
      goto error;
   }

   /* So we are recursively re-locking a lock we already w-hold. */
   lockN_acquire_writer( lk, thr );
   goto noerror;

  noerror:
   /* check lock order acquisition graph, and update.  This has to
      happen before the lock is added to the thread's locksetA/W. */
   if (lk != __bus_lock_Lock)
      laog__pre_thread_acquires_lock( thr, lk );
   /* update the thread's held-locks set */
   thr->locksetA = HG_(addToWS)( univ_lsets, thr->locksetA, (Word)lk );
   thr->locksetW = HG_(addToWS)( univ_lsets, thr->locksetW, (Word)lk );

   if (mu_is_cv(lock_ga)) {
      // VG_(printf)("mu is cv: w-lock %p\n", lock_ga);
      evhH__do_cv_wait(thr, lock_ga, False);
   }

   /* fall through */

  error:
   tl_assert(is_sane_LockN(lk));
}


/* The lock at 'lock_ga' has acquired a reader.  Make all necessary
   updates, and also do all possible error checks. */
static 
void evhH__post_thread_r_acquires_lock ( Thread* thr, 
                                         LockKind lkk, Addr lock_ga )
{
   Lock* lk; 

   /* Basically what we need to do is call lockN_acquire_reader.
      However, that will barf if any 'invalid' lock states would
      result.  Therefore check before calling.  Side effect is that
      'is_sane_LockN(lk)' is both a pre- and post-condition of this
      routine. 

      Because this routine is only called after successful lock
      acquisition, we should not be asked to move the lock into any
      invalid states.  Requests to do so are bugs in libpthread, since
      that should have rejected any such requests. */

   /* be paranoid w.r.t hint bits, even if lock_ga is complete
      nonsense */
   shmem__set_mbHasLocks( lock_ga, True );

   tl_assert(is_sane_Thread(thr));
   /* Try to find the lock.  If we can't, then create a new one with
      kind 'lkk'.  Only a reader-writer lock can be read-locked,
      hence the first assertion. */
   tl_assert(lkk == LK_rdwr);
   lk = map_locks_lookup_or_create( 
           lkk, lock_ga, map_threads_reverse_lookup(thr) );
   tl_assert( is_sane_LockN(lk) );
   shmem__set_mbHasLocks( lock_ga, True );

   if (HG_(isEmptyBag)(&lk->heldBy)) {
      /* the lock isn't held.  Simple. */
      tl_assert(!lk->heldW);
      lockN_acquire_reader( lk, thr );
      goto noerror;
   }

   /* So the lock is already held.  If held as a w-lock then
      libpthread must be buggy. */
   if (lk->heldW) {
      record_error_Misc( thr, "Bug in libpthread: read lock "
                              "granted on rwlock which is "
                              "currently wr-held");
      goto error;
   }

   /* Easy enough.  In short anybody can get a read-lock on a rwlock
      provided it is either unlocked or already in rd-held. */
   lockN_acquire_reader( lk, thr );
   goto noerror;

  noerror:
   /* check lock order acquisition graph, and update.  This has to
      happen before the lock is added to the thread's locksetA/W. */
   tl_assert(lk != __bus_lock_Lock); /* bus lock only ever acquired as W */
   laog__pre_thread_acquires_lock( thr, lk );
   /* update the thread's held-locks set */
   thr->locksetA = HG_(addToWS)( univ_lsets, thr->locksetA, (Word)lk );
   /* but don't update thr->locksetW, since lk is only rd-held */

   if (mu_is_cv(lock_ga)) {
      // VG_(printf)("mu is cv: r-lock %p\n", lock_ga);
      evhH__do_cv_wait(thr, lock_ga, False);
   }

   /* fall through */

  error:
   tl_assert(is_sane_LockN(lk));
}


/* The lock at 'lock_ga' is just about to be unlocked.  Make all
   necessary updates, and also do all possible error checks. */
static 
void evhH__pre_thread_releases_lock ( Thread* thr,
                                      Addr lock_ga, Bool isRDWR )
{
   Lock* lock;
   UWord n;

   /* This routine is called prior to a lock release, before
      libpthread has had a chance to validate the call.  Hence we need
      to detect and reject any attempts to move the lock into an
      invalid state.  Such attempts are bugs in the client.

      isRDWR is True if we know from the wrapper context that lock_ga
      should refer to a reader-writer lock, and is False if [ditto]
      lock_ga should refer to a standard mutex. */

   /* be paranoid w.r.t hint bits, even if lock_ga is complete
      nonsense */
   shmem__set_mbHasLocks( lock_ga, True );

   tl_assert(is_sane_Thread(thr));
   lock = map_locks_maybe_lookup( lock_ga );

   if (!lock) {
      /* We know nothing about a lock at 'lock_ga'.  Nevertheless
         the client is trying to unlock it.  So complain, then ignore
         the attempt. */
      record_error_UnlockBogus( thr, lock_ga );
      return;
   }

   tl_assert(lock->guestaddr == lock_ga);
   tl_assert(is_sane_LockN(lock));

   if (isRDWR && lock->kind != LK_rdwr) {
      record_error_Misc( thr, "pthread_rwlock_unlock with a "
                              "pthread_mutex_t* argument " );
   }
   if ((!isRDWR) && lock->kind == LK_rdwr) {
      record_error_Misc( thr, "pthread_mutex_unlock with a "
                              "pthread_rwlock_t* argument " );
   }

   if (HG_(isEmptyBag)(&lock->heldBy)) {
      /* The lock is not held.  This indicates a serious bug in the
         client. */
      tl_assert(!lock->heldW);
      record_error_UnlockUnlocked( thr, lock );
      tl_assert(!HG_(elemWS)( univ_lsets, thr->locksetA, (Word)lock ));
      tl_assert(!HG_(elemWS)( univ_lsets, thr->locksetW, (Word)lock ));
      goto error;
   }

   /* The lock is held.  Is this thread one of the holders?  If not,
      report a bug in the client. */
   n = HG_(elemBag)( &lock->heldBy, (Word)thr );
   tl_assert(n >= 0);
   if (n == 0) {
      /* We are not a current holder of the lock.  This is a bug in
         the guest, and (per POSIX pthread rules) the unlock
         attempt will fail.  So just complain and do nothing
         else. */
      Thread* realOwner = (Thread*)HG_(anyElementOfBag)( &lock->heldBy );
      tl_assert(is_sane_Thread(realOwner));
      tl_assert(realOwner != thr);
      tl_assert(!HG_(elemWS)( univ_lsets, thr->locksetA, (Word)lock ));
      tl_assert(!HG_(elemWS)( univ_lsets, thr->locksetW, (Word)lock ));
      record_error_UnlockForeign( thr, realOwner, lock );
      goto error;
   }

   /* Ok, we hold the lock 'n' times. */
   tl_assert(n >= 1);

   lockN_release( lock, thr );

   n--;
   tl_assert(n >= 0);

   if (n > 0) {
      tl_assert(!HG_(isEmptyBag)(&lock->heldBy));
      tl_assert(n == HG_(elemBag)( &lock->heldBy, (Word)thr )); 
      /* We still hold the lock.  So either it's a recursive lock 
         or a rwlock which is currently r-held. */
      tl_assert(lock->kind == LK_mbRec
                || (lock->kind == LK_rdwr && !lock->heldW));
      tl_assert(HG_(elemWS)( univ_lsets, thr->locksetA, (Word)lock ));
      if (lock->heldW)
         tl_assert(HG_(elemWS)( univ_lsets, thr->locksetW, (Word)lock ));
      else
         tl_assert(!HG_(elemWS)( univ_lsets, thr->locksetW, (Word)lock ));
   } else {
      /* We no longer hold the lock. */
      if (!isRDWR) {
         // This is a pure Mutex and it should not be held by anyone.
         // We can not do the same for rw-locks since a reader lock
         // can be held by other thread.
         tl_assert(HG_(isEmptyBag)(&lock->heldBy));
      }
      tl_assert(0 == HG_(elemBag)( &lock->heldBy, (Word)thr ));
      /* update this thread's lockset accordingly. */
      thr->locksetA
         = HG_(delFromWS)( univ_lsets, thr->locksetA, (Word)lock );
      thr->locksetW
         = HG_(delFromWS)( univ_lsets, thr->locksetW, (Word)lock );
   }

   if (mu_is_cv(lock_ga) ) {
      // VG_(printf)("mu is cv: unlock %p\n", lock_ga);
      evhH__do_cv_signal(thr, lock_ga);
   }
   /* fall through */

  error:
   tl_assert(is_sane_LockN(lock));
}


/*--------- Event handlers proper (evh__* functions) ---------*/

/* What is the Thread* for the currently running thread?  This is
   absolutely performance critical.  We receive notifications from the
   core for client code starts/stops, and cache the looked-up result
   in 'current_Thread'.  Hence, for the vast majority of requests,
   finding the current thread reduces to a read of a global variable,
   provided get_current_Thread_in_C_C is inlined.

   Outside of client code, current_Thread is NULL, and presumably
   any uses of it will cause a segfault.  Hence:

   - for uses definitely within client code, use
     get_current_Thread_in_C_C.

   - for all other uses, use get_current_Thread.
*/

static Thread* current_Thread = NULL;

static void evh__start_client_code ( ThreadId tid, ULong nDisp ) {
   if (0) VG_(printf)("start %d %llu\n", (Int)tid, nDisp);
   tl_assert(current_Thread == NULL);
   current_Thread = map_threads_lookup( tid );
   tl_assert(current_Thread != NULL);
}
static void evh__stop_client_code ( ThreadId tid, ULong nDisp ) {
   if (0) VG_(printf)(" stop %d %llu\n", (Int)tid, nDisp);
   tl_assert(current_Thread != NULL);
   current_Thread = NULL;
}
static inline Thread* get_current_Thread_in_C_C ( void ) {
   return current_Thread;
}
static inline Thread* get_current_Thread ( void ) {
   ThreadId coretid;
   Thread*  thr;
   thr = get_current_Thread_in_C_C();
   if (LIKELY(thr))
      return thr;
   /* evidently not in client code.  Do it the slow way. */
   coretid = VG_(get_running_tid)();
   /* FIXME: get rid of the following kludge.  It exists because
      evim__new_mem is called during initialisation (as notification
      of initial memory layout) and VG_(get_running_tid)() returns
      VG_INVALID_THREADID at that point. */
   if (coretid == VG_INVALID_THREADID)
      coretid = 1; /* KLUDGE */
   thr = map_threads_lookup( coretid );
   return thr;
}

static
void evh__new_mem ( Addr a, SizeT len ) {
   if (SHOW_EVENTS >= 2)
      VG_(printf)("evh__new_mem(%p, %lu)\n", (void*)a, len );
   shadow_mem_make_New( get_current_Thread(), a, len );
   if (len >= SCE_BIGRANGE_T && (clo_sanity_flags & SCE_BIGRANGE))
      all__sanity_check("evh__new_mem-post");
}

// Hacky stack filter; ignore references in the lowest page of
// a thread's stack.  This picks up basically all stack references
// that threads ever make (iow, ignoring the lowest 2, 3, .. pages
// hardly succeeds in filtering out any more than just the first
// page)
#define HACKY_FILTER 1
#define HACKY_FILTER_SIZE (1*4096)
// skip ref at 'a' if
// (a - sp + VG_STACK_REDZONE_SZB) <=u HACKY_FILTER_SIZE

static
void evh__new_mem_stack ( Addr a, SizeT len ) {
   if (SHOW_EVENTS >= 2)
      VG_(printf)("evh__new_mem_stack(%p, %lu)\n", (void*)a, len );
#if 0
   // This is simply wrong
   ThreadId coretid = VG_(get_running_tid)();
   if (coretid != VG_INVALID_THREADID) {
      Addr sp_min1 = VG_(get_SP)(coretid) - VG_STACK_REDZONE_SZB;
      VG_(printf)("QQQ %p\n", sp_min1);
      if (a > sp_min1 && a+len <= sp_min1 + HACKY_FILTER_SIZE) {
         VG_(printf)("XXX skip %p %lu\n", a, len);
         return;
      }
   }
#endif
#if 0
   // This isn't right either
   if (len <= HACKY_FILTER_SIZE) {
     //VG_(printf)("XXX skip %p %lu\n", a, len);
      return;
   }
   a += HACKY_FILTER_SIZE;
   len -= HACKY_FILTER_SIZE;
#endif
   shadow_mem_make_New( get_current_Thread(), a, len );
   if (len >= SCE_BIGRANGE_T && (clo_sanity_flags & SCE_BIGRANGE))
      all__sanity_check("evh__new_mem_stack-post");
}

static
void evh__new_mem_w_perms ( Addr a, SizeT len, 
                            Bool rr, Bool ww, Bool xx ) {
   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__new_mem_w_perms(%p, %lu, %d,%d,%d)\n",
                  (void*)a, len, (Int)rr, (Int)ww, (Int)xx );
   if (rr || ww || xx)
      shadow_mem_make_New( get_current_Thread(), a, len );
   if (len >= SCE_BIGRANGE_T && (clo_sanity_flags & SCE_BIGRANGE))
      all__sanity_check("evh__new_mem_w_perms-post");
}

static
void evh__set_perms ( Addr a, SizeT len,
                      Bool rr, Bool ww, Bool xx ) {
   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__set_perms(%p, %lu, %d,%d,%d)\n",
                  (void*)a, len, (Int)rr, (Int)ww, (Int)xx );
   /* Hmm.  What should we do here, that actually makes any sense?
      Let's say: if neither readable nor writable, then declare it
      NoAccess, else leave it alone. */
   if (!(rr || ww))
      shadow_mem_make_NoAccess( get_current_Thread(), a, len );
   if (len >= SCE_BIGRANGE_T && (clo_sanity_flags & SCE_BIGRANGE))
      all__sanity_check("evh__set_perms-post");
}

static
void evh__die_mem ( Addr a, SizeT len ) {
   if (SHOW_EVENTS >= 2)
      VG_(printf)("evh__die_mem(%p, %lu)\n", (void*)a, len );
   shadow_mem_make_NoAccess( get_current_Thread(), a, len );
   if (len >= SCE_BIGRANGE_T && (clo_sanity_flags & SCE_BIGRANGE))
      all__sanity_check("evh__die_mem-post");
}

static
void evh__pre_thread_ll_create ( ThreadId parent, ThreadId child )
{
   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__pre_thread_ll_create(p=%d, c=%d)\n",
                  (Int)parent, (Int)child );

   if (parent != VG_INVALID_THREADID) {
      Thread*   thr_p;
      Thread*   thr_c;
      SegmentID segid_c;
      Segment*  seg_c;

      tl_assert(is_sane_ThreadId(parent));
      tl_assert(is_sane_ThreadId(child));
      tl_assert(parent != child);

      thr_p = map_threads_maybe_lookup( parent );
      thr_c = map_threads_maybe_lookup( child );

      tl_assert(thr_p != NULL);
      tl_assert(thr_c == NULL);

      /* Create a new thread record for the child. */
      // FIXME: code duplication from init_data_structures
      segid_c   = mk_Segment( NULL/*thr*/, NULL/*prev*/, NULL/*other*/ );
      seg_c     = SEG_get(segid_c);

      /* a Thread for the new thread and bind it in the thread-map table */
      thr_c = mk_Thread( segid_c, child );
      seg_c->thr = thr_c;

      /* Record where the parent is so we can later refer to this in
         error messages.

         On amd64-linux, this entails a nasty glibc-2.5 specific hack.
         The stack snapshot is taken immediately after the parent has
         returned from its sys_clone call.  Unfortunately there is no
         unwind info for the insn following "syscall" - reading the
         glibc sources confirms this.  So we ask for a snapshot to be
         taken as if RIP was 3 bytes earlier, in a place where there
         is unwind info.  Sigh.
      */
      { Word first_ip_delta = 0;
#       if defined(VGP_amd64_linux)
        first_ip_delta = -3;
#       endif
        thr_c->created_at = VG_(record_ExeContext)(parent, first_ip_delta);

        if (clo_trace_level >= 1) {
           VG_(message)(Vg_UserMsg, "Created thread: T%d", thr_c->threadUID);
           if (clo_trace_level >= 2) {
              VG_(pp_ExeContext)(thr_c->created_at);
           }
           VG_(message)(Vg_UserMsg, "");
        }
      }

      /* Now, mess with segments. */ 
      if (clo_happens_before >= 1) {
         /* Make the child's new segment depend on the parent */
         seg_c->other = SEG_get( thr_p->csegid );
         DEBUG_ONLY(seg_c->other_hint = 'c');
         seg_c->vts = tick_VTS( thr_c, seg_c->other->vts );
         tl_assert(seg_c->prev == NULL);
         /* and start a new segment for the parent. */
         { SegmentID new_segid = 0; /* bogus */
           Segment*  new_seg   = NULL;
           evhH__start_new_segment_for_thread( &new_segid, &new_seg, 
                                               thr_p );
           tl_assert(SEG_id_is_sane(new_segid));
           tl_assert(is_sane_Segment(new_seg));
           new_seg->vts = tick_VTS( thr_p, new_seg->prev->vts );
           tl_assert(new_seg->other == NULL);
         }
      }
   }

   if (clo_sanity_flags & SCE_THREADS)
      all__sanity_check("evh__pre_thread_create-post");
}

static
void evh__pre_thread_ll_exit ( ThreadId quit_tid )
{
   Int     nHeld;
   Thread* thr_q;
   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__pre_thread_ll_exit(thr=%d)\n",
                  (Int)quit_tid );

   /* quit_tid has disappeared without joining to any other thread.
      Therefore there is no synchronisation event associated with its
      exit and so we have to pretty much treat it as if it was still
      alive but mysteriously making no progress.  That is because, if
      we don't know when it really exited, then we can never say there
      is a point in time when we're sure the thread really has
      finished, and so we need to consider the possibility that it
      lingers indefinitely and continues to interact with other
      threads. */
   /* However, it might have rendezvous'd with a thread that called
      pthread_join with this one as arg, prior to this point (that's
      how NPTL works).  In which case there has already been a prior
      sync event.  So in any case, just let the thread exit.  On NPTL,
      all thread exits go through here. */
   tl_assert(is_sane_ThreadId(quit_tid));
   thr_q = map_threads_maybe_lookup( quit_tid );
   tl_assert(thr_q != NULL);

   /* Complain if this thread holds any locks. */
   nHeld = HG_(cardinalityWS)( univ_lsets, thr_q->locksetA );
   tl_assert(nHeld >= 0);
   if (nHeld > 0) {
      HChar buf[80];
      VG_(sprintf)(buf, "Exiting thread still holds %d lock%s",
                        nHeld, nHeld > 1 ? "s" : "");
      record_error_Misc( thr_q, buf );
   }

   /* About the only thing we do need to do is clear the map_threads
      entry, in order that the Valgrind core can re-use it. */
   map_threads_delete( quit_tid );

   if (clo_sanity_flags & SCE_THREADS)
      all__sanity_check("evh__pre_thread_ll_exit-post");
}

static
void evh__HG_PTHREAD_JOIN_POST ( ThreadId stay_tid, Thread* quit_thr )
{
   Thread*  thr_s;
   Thread*  thr_q;

   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__post_thread_join(stayer=%d, quitter=%p)\n",
                  (Int)stay_tid, quit_thr );

   tl_assert(is_sane_ThreadId(stay_tid));

   thr_s = map_threads_maybe_lookup( stay_tid );
   thr_q = quit_thr;
   tl_assert(thr_s != NULL);
   tl_assert(thr_q != NULL);
   tl_assert(thr_s != thr_q);

   if (clo_happens_before >= 1) {
      /* Start a new segment for the stayer */
      SegmentID new_segid = 0; /* bogus */
      Segment*  new_seg   = NULL;
      evhH__start_new_segment_for_thread( &new_segid, &new_seg, thr_s );
      tl_assert(SEG_id_is_sane(new_segid));
      tl_assert(is_sane_Segment(new_seg));
      /* and make it depend on the quitter's last segment */
      tl_assert(new_seg->other == NULL);
      new_seg->other = SEG_get( thr_q->csegid );
      DEBUG_ONLY(new_seg->other_hint = 'j');
      tl_assert(new_seg->thr == thr_s);
      new_seg->vts = tickL_and_joinR_VTS( thr_s, new_seg->prev->vts,
                                                 new_seg->other->vts );
   }

      /* This holds because, at least when using NPTL as the thread
      library, we should be notified the low level thread exit before
      we hear of any join event on it.  The low level exit
      notification feeds through into evh__pre_thread_ll_exit,
      which should clear the map_threads entry for it.  Hence we
      expect there to be no map_threads entry at this point. */
   tl_assert( map_threads_maybe_reverse_lookup(thr_q)
              == VG_INVALID_THREADID);

   if (clo_sanity_flags & SCE_THREADS)
      all__sanity_check("evh__post_thread_join-post");
}

static
void evh__pre_mem_read ( CorePart part, ThreadId tid, Char* s, 
                         Addr a, SizeT size) {
   if (SHOW_EVENTS >= 2
       || (SHOW_EVENTS >= 1 && size != 1))
      VG_(printf)("evh__pre_mem_read(ctid=%d, \"%s\", %p, %lu)\n", 
                  (Int)tid, s, (void*)a, size );
   shadow_mem_read_range( map_threads_lookup(tid), a, size);
   if (size >= SCE_BIGRANGE_T && (clo_sanity_flags & SCE_BIGRANGE))
      all__sanity_check("evh__pre_mem_read-post");
}

static
void evh__pre_mem_read_asciiz ( CorePart part, ThreadId tid,
                                Char* s, Addr a ) {
   Int len;
   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__pre_mem_asciiz(ctid=%d, \"%s\", %p)\n", 
                  (Int)tid, s, (void*)a );
   // FIXME: think of a less ugly hack
   len = VG_(strlen)( (Char*) a );
   shadow_mem_read_range( map_threads_lookup(tid), a, len+1 );
   if (len >= SCE_BIGRANGE_T && (clo_sanity_flags & SCE_BIGRANGE))
      all__sanity_check("evh__pre_mem_read_asciiz-post");
}

static
void evh__pre_mem_write ( CorePart part, ThreadId tid, Char* s,
                          Addr a, SizeT size ) {
   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__pre_mem_write(ctid=%d, \"%s\", %p, %lu)\n", 
                  (Int)tid, s, (void*)a, size );
   shadow_mem_write_range( map_threads_lookup(tid), a, size);
   if (size >= SCE_BIGRANGE_T && (clo_sanity_flags & SCE_BIGRANGE))
      all__sanity_check("evh__pre_mem_write-post");
}

static
void evh__new_mem_heap ( Addr a, SizeT len, Bool is_inited ) {
   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__new_mem_heap(%p, %lu, inited=%d)\n", 
                  (void*)a, len, (Int)is_inited );
   // FIXME: this is kinda stupid
   if (is_inited) {
      shadow_mem_make_New(get_current_Thread(), a, len);
   } else {
      shadow_mem_make_New(get_current_Thread(), a, len);
   }
   if (len >= SCE_BIGRANGE_T && (clo_sanity_flags & SCE_BIGRANGE))
      all__sanity_check("evh__pre_mem_read-post");
}

static
void evh__die_mem_heap ( Addr a, SizeT len ) {
   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__die_mem_heap(%p, %lu)\n", (void*)a, len );
   shadow_mem_make_NoAccess( get_current_Thread(), a, len );
   if (len >= SCE_BIGRANGE_T && (clo_sanity_flags & SCE_BIGRANGE))
      all__sanity_check("evh__pre_mem_read-post");
}

// thread async exit?

// skip ref at 'a' if
// (a - sp + VG_STACK_REDZONE_SZB) <=u HACKY_FILTER_SIZE

static VG_REGPARM(2)
void evh__mem_help_read_1(Addr a, Addr sp) {
   if (HACKY_FILTER 
       && ((UWord)(a - sp + VG_STACK_REDZONE_SZB)) <= HACKY_FILTER_SIZE)
      return;
   shadow_mem_read8( get_current_Thread_in_C_C(), a, 0/*unused*/ );
}
static VG_REGPARM(2)
void evh__mem_help_read_2(Addr a, Addr sp) {
   if (HACKY_FILTER 
       && ((UWord)(a - sp + VG_STACK_REDZONE_SZB)) <= HACKY_FILTER_SIZE)
      return;
   shadow_mem_read16( get_current_Thread_in_C_C(), a, 0/*unused*/ );
}
static VG_REGPARM(2)
void evh__mem_help_read_4(Addr a, Addr sp) {
   if (HACKY_FILTER 
       && ((UWord)(a - sp + VG_STACK_REDZONE_SZB)) <= HACKY_FILTER_SIZE)
      return;
   shadow_mem_read32( get_current_Thread_in_C_C(), a, 0/*unused*/ );
}
static VG_REGPARM(2)
void evh__mem_help_read_8(Addr a, Addr sp) {
   if (HACKY_FILTER 
       && ((UWord)(a - sp + VG_STACK_REDZONE_SZB)) <= HACKY_FILTER_SIZE)
      return;
   shadow_mem_read64( get_current_Thread_in_C_C(), a, 0/*unused*/ );
}
static VG_REGPARM(3)
void evh__mem_help_read_N(Addr a, SizeT size, Addr sp) {
   if (HACKY_FILTER 
       && ((UWord)(a - sp + VG_STACK_REDZONE_SZB)) <= HACKY_FILTER_SIZE)
      return;
   shadow_mem_read_range( get_current_Thread_in_C_C(), a, size );
}

static VG_REGPARM(2)
void evh__mem_help_write_1(Addr a, Addr sp) {
   if (HACKY_FILTER
       && ((UWord)(a - sp + VG_STACK_REDZONE_SZB)) <= HACKY_FILTER_SIZE)
      return;
   shadow_mem_write8( get_current_Thread_in_C_C(), a, 0/*unused*/ );
}
static VG_REGPARM(2)
void evh__mem_help_write_2(Addr a, Addr sp) {
   if (HACKY_FILTER
       && ((UWord)(a - sp + VG_STACK_REDZONE_SZB)) <= HACKY_FILTER_SIZE)
      return;
   shadow_mem_write16( get_current_Thread_in_C_C(), a, 0/*unused*/ );
}
static VG_REGPARM(2)
void evh__mem_help_write_4(Addr a, Addr sp) {
   if (HACKY_FILTER
       && ((UWord)(a - sp + VG_STACK_REDZONE_SZB)) <= HACKY_FILTER_SIZE)
      return;
   shadow_mem_write32( get_current_Thread_in_C_C(), a, 0/*unused*/ );
}
static VG_REGPARM(2)
void evh__mem_help_write_8(Addr a, Addr sp) {
   if (HACKY_FILTER
       && ((UWord)(a - sp + VG_STACK_REDZONE_SZB)) <= HACKY_FILTER_SIZE)
      return;
   shadow_mem_write64( get_current_Thread_in_C_C(), a, 0/*unused*/ );
}
static VG_REGPARM(3)
void evh__mem_help_write_N(Addr a, SizeT size, Addr sp) {
   if (HACKY_FILTER
       && ((UWord)(a - sp + VG_STACK_REDZONE_SZB)) <= HACKY_FILTER_SIZE)
      return;
   shadow_mem_write_range( get_current_Thread_in_C_C(), a, size );
}

static void evh__bus_lock(void) {
   Thread* thr;
   if (0) VG_(printf)("evh__bus_lock()\n");
   thr = get_current_Thread();
   tl_assert(thr); /* cannot fail - Thread* must already exist */
   evhH__post_thread_w_acquires_lock( thr, LK_nonRec, (Addr)&__bus_lock );
}
static void evh__bus_unlock(void) {
   Thread* thr;
   if (0) VG_(printf)("evh__bus_unlock()\n");
   thr = get_current_Thread();
   tl_assert(thr); /* cannot fail - Thread* must already exist */
   evhH__pre_thread_releases_lock( thr, (Addr)&__bus_lock, False/*!isRDWR*/ );
}


/* -------------- events to do with mutexes -------------- */

/* EXPOSITION only: by intercepting lock init events we can show the
   user where the lock was initialised, rather than only being able to
   show where it was first locked.  Intercepting lock initialisations
   is not necessary for the basic operation of the race checker. */
static
void evh__HG_PTHREAD_MUTEX_INIT_POST( ThreadId tid, 
                                      void* mutex, Word mbRec )
{
   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__hg_PTHREAD_MUTEX_INIT_POST(ctid=%d, mbRec=%ld, %p)\n", 
                  (Int)tid, mbRec, (void*)mutex );
   tl_assert(mbRec == 0 || mbRec == 1);
   map_locks_lookup_or_create( mbRec ? LK_mbRec : LK_nonRec,
                               (Addr)mutex, tid );
   if (clo_sanity_flags & SCE_LOCKS)
      all__sanity_check("evh__hg_PTHREAD_MUTEX_INIT_POST");
}

static
void evh__HG_PTHREAD_MUTEX_DESTROY_PRE( ThreadId tid, void* mutex )
{
   Thread* thr;
   Lock*   lk;
   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__hg_PTHREAD_MUTEX_DESTROY_PRE(ctid=%d, %p)\n", 
                  (Int)tid, (void*)mutex );

   thr = map_threads_maybe_lookup( tid );
   /* cannot fail - Thread* must already exist */
   tl_assert( is_sane_Thread(thr) );

   lk = map_locks_maybe_lookup( (Addr)mutex );

   if (lk == NULL || (lk->kind != LK_nonRec && lk->kind != LK_mbRec)) {
      record_error_Misc( thr,
                         "pthread_mutex_destroy with invalid argument" );
   }

   if (lk) {
      tl_assert( is_sane_LockN(lk) );
      tl_assert( lk->guestaddr == (Addr)mutex );
      if (!HG_(isEmptyBag)(&lk->heldBy)) {
         /* Basically act like we unlocked the lock */
         record_error_Misc( thr, "pthread_mutex_destroy of a locked mutex" );
         /* remove lock from locksets of all owning threads */
         remove_Lock_from_locksets_of_all_owning_Threads( lk );
         HG_(emptyOutBag)( &lk->heldBy );
         lk->heldW = False;
         lk->acquired_at = NULL;
      }
      tl_assert( HG_(isEmptyBag)(&lk->heldBy) );
      tl_assert( is_sane_LockN(lk) );
   }

   if (clo_sanity_flags & SCE_LOCKS)
      all__sanity_check("evh__hg_PTHREAD_MUTEX_DESTROY_PRE");
}

static void evh__HG_PTHREAD_MUTEX_LOCK_PRE ( ThreadId tid,
                                             void* mutex, Word isTryLock )
{
   /* Just check the mutex is sane; nothing else to do. */
   // 'mutex' may be invalid - not checked by wrapper
   Thread* thr;
   Lock*   lk;
   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__hg_PTHREAD_MUTEX_LOCK_PRE(ctid=%d, mutex=%p)\n", 
                  (Int)tid, (void*)mutex );

   tl_assert(isTryLock == 0 || isTryLock == 1);
   thr = map_threads_maybe_lookup( tid );
   tl_assert(thr); /* cannot fail - Thread* must already exist */

   lk = map_locks_maybe_lookup( (Addr)mutex );

   if (lk && (lk->kind == LK_rdwr)) {
      record_error_Misc( thr, "pthread_mutex_lock with a "
                              "pthread_rwlock_t* argument " );
   }

   if ( lk 
        && isTryLock == 0
        && (lk->kind == LK_nonRec || lk->kind == LK_rdwr)
        && !HG_(isEmptyBag)(&lk->heldBy)
        && lk->heldW
        && HG_(elemBag)( &lk->heldBy, (Word)thr ) > 0 ) {
      /* uh, it's a non-recursive lock and we already w-hold it, and
         this is a real lock operation (not a speculative "tryLock"
         kind of thing).  Duh.  Deadlock coming up; but at least
         produce an error message. */
      record_error_Misc( thr, "Attempt to re-lock a "
                              "non-recursive lock I already hold" );
   }
}

static void evh__HG_PTHREAD_MUTEX_LOCK_POST ( ThreadId tid, void* mutex )
{
   // only called if the real library call succeeded - so mutex is sane
   Thread* thr;
   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__HG_PTHREAD_MUTEX_LOCK_POST(ctid=%d, mutex=%p)\n", 
                  (Int)tid, (void*)mutex );

   thr = map_threads_maybe_lookup( tid );
   tl_assert(thr); /* cannot fail - Thread* must already exist */

   evhH__post_thread_w_acquires_lock( 
      thr, 
      LK_mbRec, /* if not known, create new lock with this LockKind */
      (Addr)mutex
   );
}

static void evh__HG_PTHREAD_MUTEX_UNLOCK_PRE ( ThreadId tid, void* mutex )
{
   // 'mutex' may be invalid - not checked by wrapper
   Thread* thr;
   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__HG_PTHREAD_MUTEX_UNLOCK_PRE(ctid=%d, mutex=%p)\n", 
                  (Int)tid, (void*)mutex );

   thr = map_threads_maybe_lookup( tid );
   tl_assert(thr); /* cannot fail - Thread* must already exist */

   evhH__pre_thread_releases_lock( thr, (Addr)mutex, False/*!isRDWR*/ );
}

static void evh__HG_PTHREAD_MUTEX_UNLOCK_POST ( ThreadId tid, void* mutex )
{
   // only called if the real library call succeeded - so mutex is sane
   Thread* thr;
   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__hg_PTHREAD_MUTEX_UNLOCK_POST(ctid=%d, mutex=%p)\n", 
                  (Int)tid, (void*)mutex );
   thr = map_threads_maybe_lookup( tid );
   tl_assert(thr); /* cannot fail - Thread* must already exist */

   // anything we should do here?
}


/* --------------- events to do with CVs --------------- */

/* A mapping from CV to the thread segment which has most recently
   signalled/broadcasted on it.  This makes it possible to create
   thread segments to model happens-before events arising from CV
   signallings/broadcasts.
*/

/* pthread_mutex_cond* -> Segment* */
static WordFM* map_cond_to_Segment = NULL;

static void map_cond_to_Segment_INIT ( void ) {
   if (UNLIKELY(map_cond_to_Segment == NULL)) {
      map_cond_to_Segment = HG_(newFM)( hg_zalloc, hg_free, NULL );
      tl_assert(map_cond_to_Segment != NULL);
   }
}

WordFM * signal_stat_FM = NULL;

typedef struct {
   Int  threadUID; /* VG_INVALID_THREADID if more than one thread*/
   Bool multiple_thread_access;
   UWord n_access;
} SignalInfo;

SignalInfo * new_SignalInfo (Int threadUID) {
   SignalInfo * ret = hg_zalloc(sizeof(SignalInfo));
   ret->threadUID   = threadUID;
   ret->multiple_thread_access = False;
   ret->n_access    = 1;
   return ret;
}

void record_new_fake_seg (Thread *thr, Word cond)
{
   UWord found_key, found_val;
   SignalInfo * data = NULL;
   if (signal_stat_FM == NULL)
      signal_stat_FM = HG_(newFM)( hg_zalloc, hg_free, NULL );
   
   if (HG_(lookupFM)(signal_stat_FM, &found_key, &found_val, cond)) {
      // there was a fake segment for "cond"
      data = (SignalInfo*)found_val;
      data->n_access++;
      if (!data->multiple_thread_access) {
         if (data->threadUID != thr->threadUID) {
            data->multiple_thread_access = True;
            data->threadUID = VG_INVALID_THREADID;
         } else
            tl_assert(data->threadUID != VG_INVALID_THREADID);
      } else
         tl_assert(data->threadUID == VG_INVALID_THREADID);
   } else {
      data = new_SignalInfo(thr->threadUID);
      HG_(addToFM)(signal_stat_FM, cond, (UWord)data);
   }
}

void pp_fake_segment_stats ( Int d ) {
   UWord total_conds = 0,
         conds_N_1 = 0,
         conds_N_multi = 0,
         total_fake_segments = 0,         
         N_1 = 0,
         N_multi = 0,
         iter_key, iter_value;
   SignalInfo * data = NULL;
   if (signal_stat_FM == NULL) {
      space(d); VG_(printf)("No fake segments yet...\n");
      return;
   }
   HG_(initIterFM)(signal_stat_FM);
   while (HG_(nextIterFM)(signal_stat_FM, &iter_key, &iter_value)) {
      data = (SignalInfo*)iter_value;
      total_conds++;
      total_fake_segments += data->n_access;
      if (data->multiple_thread_access) {
         conds_N_multi++;
         N_multi += data->n_access;
      } else {
         conds_N_1++;
         N_1 += data->n_access;
      }
   }
   HG_(doneIterFM)(signal_stat_FM);
   space(d); VG_(printf)("Fakes: fakes = %d, 1-thr = %d, multi-thr = %d\n",
                     total_fake_segments, N_1, N_multi);
   space(d); VG_(printf)("CVs: total = %d, 1-thr = %d, multi-thr = %d\n",
                     total_conds, conds_N_1, conds_N_multi);
}

void evhH__do_cv_signal(Thread *thr, Word cond)
{
   static Thread *fake_thread; 
   SegmentID new_segid;
   Segment*  new_seg;
   SegmentID fake_segid;
   Segment*  fake_seg;
   Segment *signalling_seg = NULL;

   map_cond_to_Segment_INIT();
   if (clo_happens_before < 2) return;
   /* create a new segment ... */
   new_segid = 0; /* bogus */
   new_seg   = NULL;
   evhH__start_new_segment_for_thread( &new_segid, &new_seg, thr );
   tl_assert( SEG_id_is_sane(new_segid) );
   tl_assert( is_sane_Segment(new_seg) );
   tl_assert( new_seg->thr == thr );
   tl_assert( is_sane_Segment(new_seg->prev) );
   tl_assert( new_seg->prev->vts );
   new_seg->vts = tick_VTS( new_seg->thr, new_seg->prev->vts );

   /* ... and add the binding. */

   if (fake_thread == NULL) {
      SegmentID segid = mk_Segment(NULL, NULL, NULL);
      Segment  *seg   = SEG_get(segid);
      fake_thread     = mk_Thread(segid, VG_INVALID_THREADID);
      seg->thr        = fake_thread;
      seg->vts        = singleton_VTS(seg->thr, 1);
   }

   record_new_fake_seg (thr, cond);
   // create a fake segment.                                         
   evhH__start_new_segment_for_thread(&fake_segid, &fake_seg, fake_thread);
   tl_assert( SEG_id_is_sane(fake_segid) );
   tl_assert( is_sane_Segment(fake_seg) );
   tl_assert( fake_seg->prev != NULL );
   tl_assert( fake_seg->other == NULL );
   fake_seg->vts = NULL;
   fake_seg->other = new_seg->prev;


   HG_(lookupFM)( map_cond_to_Segment, 
                  NULL, (Word*)&signalling_seg,
                  (Word)cond );
   if (signalling_seg != NULL) {
      // There was a signal on this CV already
      fake_seg->prev = signalling_seg;
      fake_seg->vts  = tickL_and_joinR_VTS(fake_thread, 
                                           fake_seg->prev->vts, 
                                           fake_seg->other->vts);
      stats__fake_VTS_bytes_trashed += VG_(bytesXA)(signalling_seg->vts);
      VG_(deleteXA)(signalling_seg->vts);
      signalling_seg->vts = NULL;
   } else {
      XArray * fake_VTS = new_VTS();
      ScalarTS st;
      st.thrUID = fake_thread->threadUID;
      st.tym = 1;
      VG_(addToXA)( fake_VTS, &st );
      fake_seg->prev = NULL;
      fake_seg->vts  = tickL_and_joinR_VTS(fake_thread, 
                                           fake_VTS, 
                                           fake_seg->other->vts);
      VG_(deleteXA)(fake_VTS);
   }
   HG_(addToFM)( map_cond_to_Segment, (Word)cond, (Word)(fake_seg) );
   // FIXME. test67 gives false negative. 
   // But this looks more like a feature than a bug. 
   //
   // FIXME. At this point the old signalling_seg is not needed any more
   // if we use only VTS. If we stop using HB graph, we can have only
   // one fake segment for a CV. 

}


Bool evhH__do_cv_wait(Thread *thr, Word cond, Bool must_match_signal)
{
   SegmentID new_segid;
   Segment*  new_seg;
   Segment*  signalling_seg;
   Bool      found;
   map_cond_to_Segment_INIT();
   if (clo_happens_before >= 2) {
      /* create a new segment ... */
      new_segid = 0; /* bogus */
      new_seg   = NULL;
      evhH__start_new_segment_for_thread( &new_segid, &new_seg, thr );
      tl_assert( SEG_id_is_sane(new_segid) );
      tl_assert( is_sane_Segment(new_seg) );
      tl_assert( new_seg->thr == thr );
      tl_assert( is_sane_Segment(new_seg->prev) );
      tl_assert( new_seg->other == NULL);

      /* and find out which thread signalled us; then add a dependency
         edge back to it. */
      signalling_seg = NULL;
      found = HG_(lookupFM)( map_cond_to_Segment, 
                             NULL, (Word*)&signalling_seg,
                                   (Word)cond );
      if (found) {
         tl_assert(is_sane_Segment(signalling_seg));
         tl_assert(new_seg->prev);
         tl_assert(new_seg->prev->vts);
         new_seg->other      = signalling_seg;
         DEBUG_ONLY(new_seg->other_hint = 's');
         tl_assert(new_seg->other->vts);
         new_seg->vts = tickL_and_joinR_VTS( 
                           new_seg->thr, 
                           new_seg->prev->vts,
                           new_seg->other->vts );
         return True;
      } else {
         if (must_match_signal) {
            /* Hmm.  How can a wait on 'cond' succeed if nobody signalled
               it?  If this happened it would surely be a bug in the
               threads library.  Or one of those fabled "spurious
               wakeups". */
            record_error_Misc( thr, "Bug in libpthread: pthread_cond_wait "
                               "succeeded on"
                               " without prior pthread_cond_post");
         }
         tl_assert(new_seg->prev->vts);
         new_seg->vts = tick_VTS( new_seg->thr, new_seg->prev->vts );
         return False;
      }
   }
   return False;
}


static void evh__HG_PTHREAD_COND_SIGNAL_PRE ( ThreadId tid, void* cond )
{
   /* 'tid' has signalled on 'cond'.  Start a new segment for this
      thread, and make a binding from 'cond' to our old segment in the
      mapping.  This is later used by other thread(s) which
      successfully exit from a pthread_cond_wait on the same cv; then
      they know what the signalling segment was, so a dependency edge
      back to it can be constructed. */

   Thread*   thr;

   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__HG_PTHREAD_COND_SIGNAL_PRE(ctid=%d, cond=%p)\n", 
                  (Int)tid, (void*)cond );

   map_cond_to_Segment_INIT();
   thr = map_threads_maybe_lookup( tid );
   tl_assert(thr); /* cannot fail - Thread* must already exist */

   // error-if: mutex is bogus
   // error-if: mutex is not locked


   evhH__do_cv_signal(thr, (Word)cond);
}

/* returns True if it reckons 'mutex' is valid and held by this
   thread, else False */
static Bool evh__HG_PTHREAD_COND_WAIT_PRE ( ThreadId tid,
                                            void* cond, void* mutex )
{
   Thread* thr;
   Lock*   lk;
   Bool    lk_valid = True;

   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__hg_PTHREAD_COND_WAIT_PRE"
                  "(ctid=%d, cond=%p, mutex=%p)\n", 
                  (Int)tid, (void*)cond, (void*)mutex );

   map_cond_to_Segment_INIT();
   thr = map_threads_maybe_lookup( tid );
   tl_assert(thr); /* cannot fail - Thread* must already exist */

   lk = map_locks_maybe_lookup( (Addr)mutex );

   /* Check for stupid mutex arguments.  There are various ways to be
      a bozo.  Only complain once, though, even if more than one thing
      is wrong. */
   if (lk == NULL) {
      lk_valid = False;
      record_error_Misc( 
         thr, 
         "pthread_cond_{timed}wait called with invalid mutex" );
   } else {
      tl_assert( is_sane_LockN(lk) );
      if (lk->kind == LK_rdwr) {
         lk_valid = False;
         record_error_Misc( 
            thr, "pthread_cond_{timed}wait called with mutex "
                 "of type pthread_rwlock_t*" );
      } else
         if (HG_(isEmptyBag)(&lk->heldBy)) {
         lk_valid = False;
         record_error_Misc( 
            thr, "pthread_cond_{timed}wait called with un-held mutex");
      } else
      if (!HG_(isEmptyBag)(&lk->heldBy)
          && HG_(elemBag)( &lk->heldBy, (Word)thr ) == 0) {
         lk_valid = False;
         record_error_Misc( 
            thr, "pthread_cond_{timed}wait called with mutex "
                 "held by a different thread" );
      }
   }

   // error-if: cond is also associated with a different mutex

   return lk_valid;
}

static void evh__HG_PTHREAD_COND_WAIT_POST ( ThreadId tid,
                                             void* cond, void* mutex )
{
   /* A pthread_cond_wait(cond, mutex) completed successfully.  Start
      a new segment for this thread.  Look up the signalling-segment
      for the 'cond' in the mapping, and add a dependency edge from
      the new segment back to it. */

   Thread*   thr;

   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__HG_PTHREAD_COND_WAIT_POST"
                  "(ctid=%d, cond=%p, mutex=%p)\n", 
                  (Int)tid, (void*)cond, (void*)mutex );

   map_cond_to_Segment_INIT();
   thr = map_threads_maybe_lookup( tid );
   tl_assert(thr); /* cannot fail - Thread* must already exist */

   // error-if: cond is also associated with a different mutex

   evhH__do_cv_wait(thr, (Word)cond, True);
}


/* -------------- events to do with rwlocks -------------- */

/* EXPOSITION only */
static
void evh__HG_PTHREAD_RWLOCK_INIT_POST( ThreadId tid, void* rwl )
{
   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__hg_PTHREAD_RWLOCK_INIT_POST(ctid=%d, %p)\n", 
                  (Int)tid, (void*)rwl );
   map_locks_lookup_or_create( LK_rdwr, (Addr)rwl, tid );
   if (clo_sanity_flags & SCE_LOCKS)
      all__sanity_check("evh__hg_PTHREAD_RWLOCK_INIT_POST");
}

static
void evh__HG_PTHREAD_RWLOCK_DESTROY_PRE( ThreadId tid, void* rwl )
{
   Thread* thr;
   Lock*   lk;
   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__hg_PTHREAD_RWLOCK_DESTROY_PRE(ctid=%d, %p)\n", 
                  (Int)tid, (void*)rwl );

   thr = map_threads_maybe_lookup( tid );
   /* cannot fail - Thread* must already exist */
   tl_assert( is_sane_Thread(thr) );

   lk = map_locks_maybe_lookup( (Addr)rwl );

   if (lk == NULL || lk->kind != LK_rdwr) {
      record_error_Misc( thr,
                         "pthread_rwlock_destroy with invalid argument" );
   }

   if (lk) {
      tl_assert( is_sane_LockN(lk) );
      tl_assert( lk->guestaddr == (Addr)rwl );
      if (!HG_(isEmptyBag)(&lk->heldBy)) {
         /* Basically act like we unlocked the lock */
         record_error_Misc( thr, "pthread_rwlock_destroy of a locked mutex" );
         /* remove lock from locksets of all owning threads */
         remove_Lock_from_locksets_of_all_owning_Threads( lk );
         HG_(emptyOutBag)( &lk->heldBy );
         lk->heldW = False;
         lk->acquired_at = NULL;
      }
      tl_assert( HG_(isEmptyBag)(&lk->heldBy) );
      tl_assert( is_sane_LockN(lk) );
   }

   if (clo_sanity_flags & SCE_LOCKS)
      all__sanity_check("evh__hg_PTHREAD_RWLOCK_DESTROY_PRE");
}

static 
void evh__HG_PTHREAD_RWLOCK_LOCK_PRE ( ThreadId tid,
                                       void* rwl,
                                       Word isW, Word isTryLock )
{
   /* Just check the rwl is sane; nothing else to do. */
   // 'rwl' may be invalid - not checked by wrapper
   Thread* thr;
   Lock*   lk;
   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__hg_PTHREAD_RWLOCK_LOCK_PRE(ctid=%d, isW=%d, %p)\n", 
                  (Int)tid, (Int)isW, (void*)rwl );

   tl_assert(isW == 0 || isW == 1); /* assured us by wrapper */
   tl_assert(isTryLock == 0 || isTryLock == 1); /* assured us by wrapper */
   thr = map_threads_maybe_lookup( tid );
   tl_assert(thr); /* cannot fail - Thread* must already exist */

   lk = map_locks_maybe_lookup( (Addr)rwl );
   if ( lk 
        && (lk->kind == LK_nonRec || lk->kind == LK_mbRec) ) {
      /* Wrong kind of lock.  Duh.  */
      record_error_Misc( thr, "pthread_rwlock_{rd,rw}lock with a "
                              "pthread_mutex_t* argument " );
   }
}

static 
void evh__HG_PTHREAD_RWLOCK_LOCK_POST ( ThreadId tid, void* rwl, Word isW )
{
   // only called if the real library call succeeded - so mutex is sane
   Thread* thr;
   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__hg_PTHREAD_RWLOCK_LOCK_POST(ctid=%d, isW=%d, %p)\n", 
                  (Int)tid, (Int)isW, (void*)rwl );

   tl_assert(isW == 0 || isW == 1); /* assured us by wrapper */
   thr = map_threads_maybe_lookup( tid );
   tl_assert(thr); /* cannot fail - Thread* must already exist */

   (isW ? evhH__post_thread_w_acquires_lock 
        : evhH__post_thread_r_acquires_lock)( 
      thr, 
      LK_rdwr, /* if not known, create new lock with this LockKind */
      (Addr)rwl
   );
}

static void evh__HG_PTHREAD_RWLOCK_UNLOCK_PRE ( ThreadId tid, void* rwl )
{
   // 'rwl' may be invalid - not checked by wrapper
   Thread* thr;
   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__HG_PTHREAD_RWLOCK_UNLOCK_PRE(ctid=%d, rwl=%p)\n", 
                  (Int)tid, (void*)rwl );

   thr = map_threads_maybe_lookup( tid );
   tl_assert(thr); /* cannot fail - Thread* must already exist */

   evhH__pre_thread_releases_lock( thr, (Addr)rwl, True/*isRDWR*/ );
}

static void evh__HG_PTHREAD_RWLOCK_UNLOCK_POST ( ThreadId tid, void* rwl )
{
   // only called if the real library call succeeded - so mutex is sane
   Thread* thr;
   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__hg_PTHREAD_RWLOCK_UNLOCK_POST(ctid=%d, rwl=%p)\n", 
                  (Int)tid, (void*)rwl );
   thr = map_threads_maybe_lookup( tid );
   tl_assert(thr); /* cannot fail - Thread* must already exist */

   // anything we should do here?
}


/* --------------- events to do with semaphores --------------- */

/* This is similar to but not identical to the handling for condition
   variables. */

/* For each semaphore, we maintain a stack of Segments.  When a 'post'
   operation is done on a semaphore (unlocking, essentially), a new
   segment is created for the posting thread, and the old segment is
   pushed on the semaphore's stack.

   Later, when a (probably different) thread completes 'wait' on the
   semaphore, we pop a Segment off the semaphore's stack (which should
   be nonempty).  We start a new segment for the thread and make it
   also depend on the just-popped segment.  This mechanism creates
   dependencies between posters and waiters of the semaphore.

   It may not be necessary to use a stack - perhaps a bag of Segments
   would do.  But we do need to keep track of how many unused-up posts
   have happened for the semaphore.

   Imagine T1 and T2 both post once on a semphore S, and T3 waits
   twice on S.  T3 cannot complete its waits without both T1 and T2
   posting.  The above mechanism will ensure that T3 acquires
   dependencies on both T1 and T2.

   When a semaphore is initialised with value N, the initialising
   thread starts a new segment, the semaphore's stack is emptied out,
   and the old segment is pushed on the stack N times.  This allows up
   to N waits on the semaphore to acquire a dependency on the
   initialisation point, which AFAICS is the correct behaviour.

   We don't emit an error for DESTROY_PRE on a semaphore we don't know
   about.  We should.
*/

/* sem_t* -> XArray* Segment* */
static WordFM* map_sem_to_Segment_stack = NULL;

static void map_sem_to_Segment_stack_INIT ( void ) {
   if (map_sem_to_Segment_stack == NULL) {
      map_sem_to_Segment_stack = HG_(newFM)( hg_zalloc, hg_free, NULL );
      tl_assert(map_sem_to_Segment_stack != NULL);
   }
}

static void push_Segment_for_sem ( void* sem, Segment* seg ) {
   XArray* xa;
   tl_assert(seg);
   map_sem_to_Segment_stack_INIT();
   if (HG_(lookupFM)( map_sem_to_Segment_stack, 
                      NULL, (Word*)&xa, (Word)sem )) {
      tl_assert(xa);
      VG_(addToXA)( xa, &seg );
   } else {
      xa = VG_(newXA)( hg_zalloc, hg_free, sizeof(Segment*) );
      VG_(addToXA)( xa, &seg );
      HG_(addToFM)( map_sem_to_Segment_stack, (Word)sem, (Word)xa );
   }
}

static Segment* mb_pop_Segment_for_sem ( void* sem ) {
   XArray*  xa;
   Segment* seg;
   map_sem_to_Segment_stack_INIT();
   if (HG_(lookupFM)( map_sem_to_Segment_stack, 
                      NULL, (Word*)&xa, (Word)sem )) {
      /* xa is the stack for this semaphore. */
      Word sz = VG_(sizeXA)( xa );
      tl_assert(sz >= 0);
      if (sz == 0)
         return NULL; /* odd, the stack is empty */
      seg = *(Segment**)VG_(indexXA)( xa, sz-1 );
      tl_assert(seg);
      VG_(dropTailXA)( xa, 1 );
      return seg;
   } else {
      /* hmm, that's odd.  No stack for this semaphore. */
      return NULL;
   }
}

static void evh__HG_POSIX_SEM_DESTROY_PRE ( ThreadId tid, void* sem )
{
   Segment* seg;

   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__HG_POSIX_SEM_DESTROY_PRE(ctid=%d, sem=%p)\n", 
                  (Int)tid, (void*)sem );

   /* Empty out the semaphore's segment stack.  This way of doing it
      is stupid, but at least it's easy. */
   do {
     seg = mb_pop_Segment_for_sem( sem );
   } while (seg);

   tl_assert(!seg);
}

static 
void evh__HG_POSIX_SEM_INIT_POST ( ThreadId tid, void* sem, UWord value )
{
   Segment* seg;

   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__HG_POSIX_SEM_INIT_POST(ctid=%d, sem=%p, value=%lu)\n", 
                  (Int)tid, (void*)sem, value );

   /* Empty out the semaphore's segment stack.  This way of doing it
      is stupid, but at least it's easy. */
   do {
     seg = mb_pop_Segment_for_sem( sem );
   } while (seg);
   tl_assert(!seg);

   /* Now create a new segment for the thread, and push the old
      segment on the stack 'value' times.  Skip this if the initial
      value is zero -- no point in creating unnecessary segments. */
   if (value > 0) {
      /* create a new segment ... */
      SegmentID new_segid = 0; /* bogus */
      Segment*  new_seg   = NULL;
      Thread*   thr       = map_threads_maybe_lookup( tid );
      tl_assert(thr); /* cannot fail - Thread* must already exist */

      evhH__start_new_segment_for_thread( &new_segid, &new_seg, thr );
      tl_assert( SEG_id_is_sane(new_segid) );
      tl_assert( is_sane_Segment(new_seg) );
      tl_assert( new_seg->thr == thr );
      tl_assert( is_sane_Segment(new_seg->prev) );
      tl_assert( new_seg->prev->vts );
      new_seg->vts = tick_VTS( new_seg->thr, new_seg->prev->vts );

      if (value > 10000) {
         /* If we don't do this, the following while loop runs us out
            of memory for stupid initial values of 'sem'. */
         record_error_Misc(
            thr, "sem_init: initial value exceeds 10000; using 10000" );
         value = 10000;
      }

      while (value > 0) {
         push_Segment_for_sem( sem, new_seg->prev );
         value--;
      }
   }
}

static void evh__HG_POSIX_SEM_POST_PRE ( ThreadId tid, void* sem )
{
   /* 'tid' has posted on 'sem'.  Start a new segment for this thread,
      and push the old segment on a stack of segments associated with
      'sem'.  This is later used by other thread(s) which successfully
      exit from a sem_wait on the same sem; then they know what the
      posting segment was, so a dependency edge back to it can be
      constructed. */

   Thread*   thr;
   SegmentID new_segid;
   Segment*  new_seg;

   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__HG_POSIX_SEM_POST_PRE(ctid=%d, sem=%p)\n", 
                  (Int)tid, (void*)sem );

   thr = map_threads_maybe_lookup( tid );
   tl_assert(thr); /* cannot fail - Thread* must already exist */

   // error-if: sem is bogus

   if (clo_happens_before >= 2) {
      /* create a new segment ... */
      new_segid = 0; /* bogus */
      new_seg   = NULL;
      evhH__start_new_segment_for_thread( &new_segid, &new_seg, thr );
      tl_assert( SEG_id_is_sane(new_segid) );
      tl_assert( is_sane_Segment(new_seg) );
      tl_assert( new_seg->thr == thr );
      tl_assert( is_sane_Segment(new_seg->prev) );
      tl_assert( new_seg->prev->vts );
      new_seg->vts = tick_VTS( new_seg->thr, new_seg->prev->vts );

      /* ... and add the binding. */
      push_Segment_for_sem( sem, new_seg->prev );
   }
}

static void evh__HG_POSIX_SEM_WAIT_POST ( ThreadId tid, void* sem )
{
   /* A sem_wait(sem) completed successfully.  Start a new segment for
      this thread.  Pop the posting-segment for the 'sem' in the
      mapping, and add a dependency edge from the new segment back to
      it. */

   Thread*   thr;
   SegmentID new_segid;
   Segment*  new_seg;
   Segment*  posting_seg;

   if (SHOW_EVENTS >= 1)
      VG_(printf)("evh__HG_POSIX_SEM_WAIT_POST(ctid=%d, sem=%p)\n", 
                  (Int)tid, (void*)sem );

   thr = map_threads_maybe_lookup( tid );
   tl_assert(thr); /* cannot fail - Thread* must already exist */

   // error-if: sem is bogus

   if (clo_happens_before >= 2) {
      /* create a new segment ... */
      new_segid = 0; /* bogus */
      new_seg   = NULL;
      evhH__start_new_segment_for_thread( &new_segid, &new_seg, thr );
      tl_assert( SEG_id_is_sane(new_segid) );
      tl_assert( is_sane_Segment(new_seg) );
      tl_assert( new_seg->thr == thr );
      tl_assert( is_sane_Segment(new_seg->prev) );
      tl_assert( new_seg->other == NULL);

      /* and find out which thread posted last on sem; then add a
         dependency edge back to it. */
      posting_seg = mb_pop_Segment_for_sem( sem );
      if (posting_seg) {
         tl_assert(is_sane_Segment(posting_seg));
         tl_assert(new_seg->prev);
         tl_assert(new_seg->prev->vts);
         new_seg->other      = posting_seg;
         DEBUG_ONLY(new_seg->other_hint = 'S');
         tl_assert(new_seg->other->vts);
         new_seg->vts = tickL_and_joinR_VTS( 
                           new_seg->thr, 
                           new_seg->prev->vts,
                           new_seg->other->vts );
      } else {
         /* Hmm.  How can a wait on 'sem' succeed if nobody posted to
            it?  If this happened it would surely be a bug in the
            threads library. */
         record_error_Misc( thr, "Bug in libpthread: sem_wait succeeded on"
                                 " semaphore without prior sem_post");
         tl_assert(new_seg->prev->vts);
         new_seg->vts = tick_VTS( new_seg->thr, new_seg->prev->vts );
      }
   }
}


/*--------------------------------------------------------------*/
/*--- Lock acquisition order monitoring                      ---*/
/*--------------------------------------------------------------*/

/* FIXME: here are some optimisations still to do in
          laog__pre_thread_acquires_lock.

   The graph is structured so that if L1 --*--> L2 then L1 must be
   acquired before L2.

   The common case is that some thread T holds (eg) L1 L2 and L3 and
   is repeatedly acquiring and releasing Ln, and there is no ordering
   error in what it is doing.  Hence it repeatly:

   (1) searches laog to see if Ln --*--> {L1,L2,L3}, which always 
       produces the answer No (because there is no error).

   (2) adds edges {L1,L2,L3} --> Ln to laog, which are already present
       (because they already got added the first time T acquired Ln).

   Hence cache these two events:

   (1) Cache result of the query from last time.  Invalidate the cache
       any time any edges are added to or deleted from laog.

   (2) Cache these add-edge requests and ignore them if said edges
       have already been added to laog.  Invalidate the cache any time
       any edges are deleted from laog.
*/

typedef
   struct {
      WordSetID inns; /* in univ_laog */
      WordSetID outs; /* in univ_laog */
   }
   LAOGLinks;

/* lock order acquisition graph */
static WordFM* laog = NULL; /* WordFM Lock* LAOGLinks* */

/* EXPOSITION ONLY: for each edge in 'laog', record the two places
   where that edge was created, so that we can show the user later if
   we need to. */
typedef
   struct {
      Addr        src_ga; /* Lock guest addresses for */
      Addr        dst_ga; /* src/dst of the edge */
      ExeContext* src_ec; /* And corresponding places where that */
      ExeContext* dst_ec; /* ordering was established */
   }
   LAOGLinkExposition;


static void pp_mem_laog ( Int d ) {
   UWord laog_size_bytes = 0, m = -1;
   Lock* me;
   LAOGLinks* links;
   Int n = 0;
   if ( laog ) {
      HG_(initIterFM)( laog );
      me = NULL;
      links = NULL;
      while (HG_(nextIterFM)( laog, (Word*)&me,
                                    (Word*)&links )) {
         tl_assert(me);
         tl_assert(links);
         laog_size_bytes += 2 * sizeof(Word);
         me = NULL;
         links = NULL;
         n++;
      }
      HG_(doneIterFM)( laog );
   }
      
   laog_size_bytes = HG_(memoryConsumedWSU) (univ_laog, &m);
   space(d); VG_(printf)("laog:     %7d kB (%d laog, "
               "univ_laog: active = %d, total = %d)\n", 
         (int)(laog_size_bytes/1024), n, m, HG_(cardinalityWSU) (univ_laog));
}
   
static Word cmp_LAOGLinkExposition ( UWord llx1W, UWord llx2W ) {
   /* Compare LAOGLinkExposition*s by (src_ga,dst_ga) field pair. */
   LAOGLinkExposition* llx1 = (LAOGLinkExposition*)llx1W;
   LAOGLinkExposition* llx2 = (LAOGLinkExposition*)llx2W;
   if (llx1->src_ga < llx2->src_ga) return -1;
   if (llx1->src_ga > llx2->src_ga) return  1;
   if (llx1->dst_ga < llx2->dst_ga) return -1;
   if (llx1->dst_ga > llx2->dst_ga) return  1;
   return 0;
}

static WordFM* laog_exposition = NULL; /* WordFM LAOGLinkExposition* NULL */
/* end EXPOSITION ONLY */


static void laog__show ( Char* who ) {
   Word i, ws_size;
   UWord* ws_words;
   Lock* me;
   LAOGLinks* links;
   VG_(printf)("laog (requested by %s) {\n", who);
   HG_(initIterFM)( laog );
   me = NULL;
   links = NULL;
   while (HG_(nextIterFM)( laog, (Word*)&me,
                                 (Word*)&links )) {
      tl_assert(me);
      tl_assert(links);
      VG_(printf)("   node %p:\n", me);
      HG_(getPayloadWS)( &ws_words, &ws_size, univ_laog, links->inns );
      for (i = 0; i < ws_size; i++)
         VG_(printf)("      inn %p\n", ws_words[i] );
      HG_(getPayloadWS)( &ws_words, &ws_size, univ_laog, links->outs );
      for (i = 0; i < ws_size; i++)
         VG_(printf)("      out %p\n", ws_words[i] );
      me = NULL;
      links = NULL;
   }
   HG_(doneIterFM)( laog );
   VG_(printf)("}\n");
}

__attribute__((noinline))
static void laog__add_edge ( Lock* src, Lock* dst ) {
   Word       keyW;
   LAOGLinks* links;
   Bool       presentF, presentR;
   if (0) VG_(printf)("laog__add_edge %p %p\n", src, dst);

   /* Take the opportunity to sanity check the graph.  Record in
      presentF if there is already a src->dst mapping in this node's
      forwards links, and presentR if there is already a src->dst
      mapping in this node's backwards links.  They should agree!
      Also, we need to know whether the edge was already present so as
      to decide whether or not to update the link details mapping.  We
      can compute presentF and presentR essentially for free, so may
      as well do this always. */
   presentF = presentR = False;

   /* Update the out edges for src */
   keyW  = 0;
   links = NULL;
   if (HG_(lookupFM)( laog, &keyW, (Word*)&links, (Word)src )) {
      WordSetID outs_new;
      tl_assert(links);
      tl_assert(keyW == (Word)src);
      outs_new = HG_(addToWS)( univ_laog, links->outs, (Word)dst );
      presentF = outs_new == links->outs;
      links->outs = outs_new;
   } else {
      links = hg_zalloc(sizeof(LAOGLinks));
      links->inns = HG_(emptyWS)( univ_laog );
      links->outs = HG_(singletonWS)( univ_laog, (Word)dst );
      HG_(addToFM)( laog, (Word)src, (Word)links );
   }
   /* Update the in edges for dst */
   keyW  = 0;
   links = NULL;
   if (HG_(lookupFM)( laog, &keyW, (Word*)&links, (Word)dst )) {
      WordSetID inns_new;
      tl_assert(links);
      tl_assert(keyW == (Word)dst);
      inns_new = HG_(addToWS)( univ_laog, links->inns, (Word)src );
      presentR = inns_new == links->inns;
      links->inns = inns_new;
   } else {
      links = hg_zalloc(sizeof(LAOGLinks));
      links->inns = HG_(singletonWS)( univ_laog, (Word)src );
      links->outs = HG_(emptyWS)( univ_laog );
      HG_(addToFM)( laog, (Word)dst, (Word)links );
   }

   tl_assert( (presentF && presentR) || (!presentF && !presentR) );

   if (!presentF && src->acquired_at && dst->acquired_at) {
      LAOGLinkExposition expo;
      /* If this edge is entering the graph, and we have acquired_at
         information for both src and dst, record those acquisition
         points.  Hence, if there is later a violation of this
         ordering, we can show the user the two places in which the
         required src-dst ordering was previously established. */
      if (0) VG_(printf)("acquire edge %p %p\n", 
                         src->guestaddr, dst->guestaddr);
      expo.src_ga = src->guestaddr;
      expo.dst_ga = dst->guestaddr;
      expo.src_ec = NULL;
      expo.dst_ec = NULL;
      tl_assert(laog_exposition);
      if (HG_(lookupFM)( laog_exposition, NULL, NULL, (Word)&expo )) {
         /* we already have it; do nothing */
      } else {
         LAOGLinkExposition* expo2 = hg_zalloc(sizeof(LAOGLinkExposition));
         expo2->src_ga = src->guestaddr;
         expo2->dst_ga = dst->guestaddr;
         expo2->src_ec = src->acquired_at;
         expo2->dst_ec = dst->acquired_at;
         HG_(addToFM)( laog_exposition, (Word)expo2, (Word)NULL );
      }
   }
}

__attribute__((noinline))
static void laog__del_edge ( Lock* src, Lock* dst ) {
   Word       keyW;
   LAOGLinks* links;
   if (0) VG_(printf)("laog__del_edge %p %p\n", src, dst);
   /* Update the out edges for src */
   keyW  = 0;
   links = NULL;
   if (HG_(lookupFM)( laog, &keyW, (Word*)&links, (Word)src )) {
      tl_assert(links);
      tl_assert(keyW == (Word)src);
      links->outs = HG_(delFromWS)( univ_laog, links->outs, (Word)dst );
   }
   /* Update the in edges for dst */
   keyW  = 0;
   links = NULL;
   if (HG_(lookupFM)( laog, &keyW, (Word*)&links, (Word)dst )) {
      tl_assert(links);
      tl_assert(keyW == (Word)dst);
      links->inns = HG_(delFromWS)( univ_laog, links->inns, (Word)src );
   }
}

__attribute__((noinline))
static WordSetID /* in univ_laog */ laog__succs ( Lock* lk ) {
   Word       keyW;
   LAOGLinks* links;
   keyW  = 0;
   links = NULL;
   if (HG_(lookupFM)( laog, &keyW, (Word*)&links, (Word)lk )) {
      tl_assert(links);
      tl_assert(keyW == (Word)lk);
      return links->outs;
   } else {
      return HG_(emptyWS)( univ_laog );
   }
}

__attribute__((noinline))
static WordSetID /* in univ_laog */ laog__preds ( Lock* lk ) {
   Word       keyW;
   LAOGLinks* links;
   keyW  = 0;
   links = NULL;
   if (HG_(lookupFM)( laog, &keyW, (Word*)&links, (Word)lk )) {
      tl_assert(links);
      tl_assert(keyW == (Word)lk);
      return links->inns;
   } else {
      return HG_(emptyWS)( univ_laog );
   }
}

__attribute__((noinline))
static void laog__sanity_check ( Char* who ) {
   Word i, ws_size;
   UWord* ws_words;
   Lock* me;
   LAOGLinks* links;
   if ( !laog )
      return; /* nothing much we can do */
   HG_(initIterFM)( laog );
   me = NULL;
   links = NULL;
   if (0) VG_(printf)("laog sanity check\n");
   while (HG_(nextIterFM)( laog, (Word*)&me,
                                 (Word*)&links )) {
      tl_assert(me);
      tl_assert(links);
      HG_(getPayloadWS)( &ws_words, &ws_size, univ_laog, links->inns );
      for (i = 0; i < ws_size; i++) {
         if ( ! HG_(elemWS)( univ_laog, 
                             laog__succs( (Lock*)ws_words[i] ), 
                             (Word)me ))
            goto bad;
      }
      HG_(getPayloadWS)( &ws_words, &ws_size, univ_laog, links->outs );
      for (i = 0; i < ws_size; i++) {
         if ( ! HG_(elemWS)( univ_laog, 
                             laog__preds( (Lock*)ws_words[i] ), 
                             (Word)me ))
            goto bad;
      }
      me = NULL;
      links = NULL;
   }
   HG_(doneIterFM)( laog );
   return;

  bad:
   VG_(printf)("laog__sanity_check(%s) FAILED\n", who);
   laog__show(who);
   tl_assert(0);
}

/* If there is a path in laog from 'src' to any of the elements in
   'dst', return an arbitrarily chosen element of 'dst' reachable from
   'src'.  If no path exist from 'src' to any element in 'dst', return
   NULL. */
__attribute__((noinline))
static
Lock* laog__do_dfs_from_to ( Lock* src, WordSetID dsts /* univ_lsets */ )
{
   Lock*     ret;
   Word      i, ssz;
   XArray*   stack;   /* of Lock* */
   WordFM*   visited; /* Lock* -> void, iow, Set(Lock*) */
   Lock*     here;
   WordSetID succs;
   Word      succs_size;
   UWord*    succs_words;
   //laog__sanity_check();

   /* If the destination set is empty, we can never get there from
      'src' :-), so don't bother to try */
   if (HG_(isEmptyWS)( univ_lsets, dsts ))
      return NULL;

   ret     = NULL;
   stack   = VG_(newXA)( hg_zalloc, hg_free, sizeof(Lock*) );
   visited = HG_(newFM)( hg_zalloc, hg_free, NULL/*unboxedcmp*/ );

   (void) VG_(addToXA)( stack, &src );

   while (True) {

      ssz = VG_(sizeXA)( stack );

      if (ssz == 0) { ret = NULL; break; }

      here = *(Lock**) VG_(indexXA)( stack, ssz-1 );
      VG_(dropTailXA)( stack, 1 );

      if (HG_(elemWS)( univ_lsets, dsts, (Word)here )) { ret = here; break; }

      if (HG_(lookupFM)( visited, NULL, NULL, (Word)here ))
         continue;

      HG_(addToFM)( visited, (Word)here, 0 );

      succs = laog__succs( here );
      HG_(getPayloadWS)( &succs_words, &succs_size, univ_laog, succs );
      for (i = 0; i < succs_size; i++)
         (void) VG_(addToXA)( stack, &succs_words[i] );
   }

   HG_(deleteFM)( visited, NULL, NULL );
   VG_(deleteXA)( stack );
   return ret;
}


/* Thread 'thr' is acquiring 'lk'.  Check for inconsistent ordering
   between 'lk' and the locks already held by 'thr' and issue a
   complaint if so.  Also, update the ordering graph appropriately.
*/
__attribute__((noinline))
static void laog__pre_thread_acquires_lock ( 
               Thread* thr, /* NB: BEFORE lock is added */
               Lock*   lk
            )
{
   UWord*   ls_words;
   Word     ls_size, i;
   Lock*    other;

   /* There's no point in tracking the BHL with this mechanism, as
      that just wastes time, and the lock is always at the "top" of
      any hierarchy in which it appears (since it is only transiently
      locked for one instruction).  So just insist the callers don't
      provide it. */
   if (UNLIKELY(lk == __bus_lock_Lock)) {
      tl_assert(lk->guestaddr == (Addr)&__bus_lock); /* as it should be */
      tl_assert(0); /* caller should never supply this arg */
   } else {
      tl_assert(lk->guestaddr != (Addr)&__bus_lock);
   }

   /* It may be that 'thr' already holds 'lk' and is recursively
      relocking in.  In this case we just ignore the call. */
   /* NB: univ_lsets really is correct here */
   if (HG_(elemWS)( univ_lsets, thr->locksetA, (Word)lk ))
      return;

   if (!laog)
      laog = HG_(newFM)( hg_zalloc, hg_free, NULL/*unboxedcmp*/ );
   if (!laog_exposition)
      laog_exposition = HG_(newFM)( hg_zalloc, hg_free, 
                                    cmp_LAOGLinkExposition );

   /* First, the check.  Complain if there is any path in laog from lk
      to any of the locks already held by thr, since if any such path
      existed, it would mean that previously lk was acquired before
      (rather than after, as we are doing here) at least one of those
      locks.
   */
   other = laog__do_dfs_from_to(lk, thr->locksetA);
   if (other) {
      LAOGLinkExposition key, *found;
      /* So we managed to find a path lk --*--> other in the graph,
         which implies that 'lk' should have been acquired before
         'other' but is in fact being acquired afterwards.  We present
         the lk/other arguments to record_error_LockOrder in the order
         in which they should have been acquired. */
      /* Go look in the laog_exposition mapping, to find the allocation
         points for this edge, so we can show the user. */
      key.src_ga = lk->guestaddr;
      key.dst_ga = other->guestaddr;
      key.src_ec = NULL;
      key.dst_ec = NULL;
      found = NULL;
      if (HG_(lookupFM)( laog_exposition,
                         (Word*)&found, NULL, (Word)&key )) {
         tl_assert(found != &key);
         tl_assert(found->src_ga == key.src_ga);
         tl_assert(found->dst_ga == key.dst_ga);
         tl_assert(found->src_ec);
         tl_assert(found->dst_ec);
         record_error_LockOrder( thr, 
                                 lk->guestaddr, other->guestaddr,
                                 found->src_ec, found->dst_ec );
      } else {
         /* Hmm.  This can't happen (can it?) */
         record_error_LockOrder( thr, 
                                 lk->guestaddr,        other->guestaddr,
                                 NULL, NULL );
      }
   }

   /* Second, add to laog the pairs
        (old, lk)  |  old <- locks already held by thr
      Since both old and lk are currently held by thr, their acquired_at
      fields must be non-NULL.
   */
   tl_assert(lk->acquired_at);
   HG_(getPayloadWS)( &ls_words, &ls_size, univ_lsets, thr->locksetA );
   for (i = 0; i < ls_size; i++) {
      Lock* old = (Lock*)ls_words[i];
      tl_assert(old->acquired_at);
      laog__add_edge( old, lk );
   }

   /* Why "except_Locks" ?  We're here because a lock is being
      acquired by a thread, and we're in an inconsistent state here.
      See the call points in evhH__post_thread_{r,w}_acquires_lock.
      When called in this inconsistent state, locks__sanity_check duly
      barfs. */
   if (clo_sanity_flags & SCE_LAOG)
      all_except_Locks__sanity_check("laog__pre_thread_acquires_lock-post");
}


/* Delete from 'laog' any pair mentioning a lock in locksToDelete */

__attribute__((noinline))
static void laog__handle_one_lock_deletion ( Lock* lk )
{
   WordSetID preds, succs;
   Word preds_size, succs_size, i, j;
   UWord *preds_words, *succs_words;

   preds = laog__preds( lk );
   succs = laog__succs( lk );

   HG_(getPayloadWS)( &preds_words, &preds_size, univ_laog, preds );
   for (i = 0; i < preds_size; i++)
      laog__del_edge( (Lock*)preds_words[i], lk );

   HG_(getPayloadWS)( &succs_words, &succs_size, univ_laog, succs );
   for (j = 0; j < succs_size; j++)
      laog__del_edge( lk, (Lock*)succs_words[j] );

   for (i = 0; i < preds_size; i++) {
      for (j = 0; j < succs_size; j++) {
         if (preds_words[i] != succs_words[j]) {
            /* This can pass unlocked locks to laog__add_edge, since
               we're deleting stuff.  So their acquired_at fields may
               be NULL. */
            laog__add_edge( (Lock*)preds_words[i], (Lock*)succs_words[j] );
         }
      }
   }
}

__attribute__((noinline))
static void laog__handle_lock_deletions (
               WordSetID /* in univ_laog */ locksToDelete
            )
{
   Word   i, ws_size;
   UWord* ws_words;

   if (!laog)
      laog = HG_(newFM)( hg_zalloc, hg_free, NULL/*unboxedcmp*/ );
   if (!laog_exposition)
      laog_exposition = HG_(newFM)( hg_zalloc, hg_free, 
                                    cmp_LAOGLinkExposition );

   HG_(getPayloadWS)( &ws_words, &ws_size, univ_lsets, locksToDelete );
   for (i = 0; i < ws_size; i++)
      laog__handle_one_lock_deletion( (Lock*)ws_words[i] );

   if (clo_sanity_flags & SCE_LAOG)
      all__sanity_check("laog__handle_lock_deletions-post");
}


/*--------------------------------------------------------------*/
/*--- Malloc/free replacements                               ---*/
/*--------------------------------------------------------------*/

typedef
   struct {
      void*       next;    /* required by m_hashtable */
      Addr        payload; /* ptr to actual block    */
      SizeT       szB;     /* size requested         */
      ExeContext* where;   /* where it was allocated */
      Thread*     thr;     /* allocating thread      */
   }
   MallocMeta;

/* A hash table of MallocMetas, used to track malloc'd blocks
   (obviously). */
static VgHashTable hg_mallocmeta_table = NULL;


static MallocMeta* new_MallocMeta ( void ) {
   MallocMeta* md = hg_zalloc( sizeof(MallocMeta) );
   tl_assert(md);
   return md;
}
static void delete_MallocMeta ( MallocMeta* md ) {
   hg_free(md);
}


/* Allocate a client block and set up the metadata for it. */

static
void* handle_alloc ( ThreadId tid, 
                     SizeT szB, SizeT alignB, Bool is_zeroed )
{
   Addr        p;
   MallocMeta* md;

   tl_assert( ((SSizeT)szB) >= 0 );
   p = (Addr)VG_(cli_malloc)(alignB, szB);
   if (!p) {
      return NULL;
   }
   if (is_zeroed)
      VG_(memset)((void*)p, 0, szB);

   /* Note that map_threads_lookup must succeed (cannot assert), since
      memory can only be allocated by currently alive threads, hence
      they must have an entry in map_threads. */
   md = new_MallocMeta();
   md->payload = p;
   md->szB     = szB;
   md->where   = VG_(record_ExeContext)( tid, 0 );
   md->thr     = map_threads_lookup( tid );

   VG_(HT_add_node)( hg_mallocmeta_table, (VgHashNode*)md );

   /* Tell the lower level memory wranglers. */
   evh__new_mem_heap( p, szB, is_zeroed );

   return (void*)p;
}

/* Re the checks for less-than-zero (also in hg_cli__realloc below):
   Cast to a signed type to catch any unexpectedly negative args.
   We're assuming here that the size asked for is not greater than
   2^31 bytes (for 32-bit platforms) or 2^63 bytes (for 64-bit
   platforms). */
static void* hg_cli__malloc ( ThreadId tid, SizeT n ) {
   if (((SSizeT)n) < 0) return NULL;
   return handle_alloc ( tid, n, VG_(clo_alignment),
                         /*is_zeroed*/False );
}
static void* hg_cli____builtin_new ( ThreadId tid, SizeT n ) {
   if (((SSizeT)n) < 0) return NULL;
   return handle_alloc ( tid, n, VG_(clo_alignment),
                         /*is_zeroed*/False );
}
static void* hg_cli____builtin_vec_new ( ThreadId tid, SizeT n ) {
   if (((SSizeT)n) < 0) return NULL;
   return handle_alloc ( tid, n, VG_(clo_alignment), 
                         /*is_zeroed*/False );
}
static void* hg_cli__memalign ( ThreadId tid, SizeT align, SizeT n ) {
   if (((SSizeT)n) < 0) return NULL;
   return handle_alloc ( tid, n, align, 
                         /*is_zeroed*/False );
}
static void* hg_cli__calloc ( ThreadId tid, SizeT nmemb, SizeT size1 ) {
   if ( ((SSizeT)nmemb) < 0 || ((SSizeT)size1) < 0 ) return NULL;
   return handle_alloc ( tid, nmemb*size1, VG_(clo_alignment),
                         /*is_zeroed*/True );
}


/* Free a client block, including getting rid of the relevant
   metadata. */

static void handle_free ( ThreadId tid, void* p )
{
   MallocMeta *md, *old_md;
   SizeT      szB;

   /* First see if we can find the metadata for 'p'. */
   md = (MallocMeta*) VG_(HT_lookup)( hg_mallocmeta_table, (UWord)p );
   if (!md)
      return; /* apparently freeing a bogus address.  Oh well. */

   tl_assert(md->payload == (Addr)p);
   szB = md->szB;

   /* Nuke the metadata block */
   old_md = (MallocMeta*)
            VG_(HT_remove)( hg_mallocmeta_table, (UWord)p );
   tl_assert(old_md); /* it must be present - we just found it */
   tl_assert(old_md == md);
   tl_assert(old_md->payload == (Addr)p);

   VG_(cli_free)((void*)old_md->payload);
   delete_MallocMeta(old_md);

   /* Tell the lower level memory wranglers. */
   evh__die_mem_heap( (Addr)p, szB );
}

static void hg_cli__free ( ThreadId tid, void* p ) {
   handle_free(tid, p);
}
static void hg_cli____builtin_delete ( ThreadId tid, void* p ) {
   handle_free(tid, p);
}
static void hg_cli____builtin_vec_delete ( ThreadId tid, void* p ) {
   handle_free(tid, p);
}


static void* hg_cli__realloc ( ThreadId tid, void* payloadV, SizeT new_size )
{
   MallocMeta *md, *md_new, *md_tmp;
   SizeT      i;

   Addr payload = (Addr)payloadV;

   if (((SSizeT)new_size) < 0) return NULL;

   md = (MallocMeta*) VG_(HT_lookup)( hg_mallocmeta_table, (UWord)payload );
   if (!md)
      return NULL; /* apparently realloc-ing a bogus address.  Oh well. */
  
   tl_assert(md->payload == payload);

   if (md->szB == new_size) {
      /* size unchanged */
      md->where = VG_(record_ExeContext)(tid, 0);
      return payloadV;
   }

   if (md->szB > new_size) {
      /* new size is smaller */
      md->szB   = new_size;
      md->where = VG_(record_ExeContext)(tid, 0);
      evh__die_mem_heap( md->payload + new_size, md->szB - new_size );
      return payloadV;
   }

   /* else */ {
      /* new size is bigger */
      Addr p_new = (Addr)VG_(cli_malloc)(VG_(clo_alignment), new_size);

      /* First half kept and copied, second half new */
      // FIXME: shouldn't we use a copier which implements the
      // memory state machine?
      shadow_mem_copy_range( payload, p_new, md->szB );
      evh__new_mem_heap ( p_new + md->szB, new_size - md->szB,
                           /*inited*/False );
      /* FIXME: can anything funny happen here?  specifically, if the
         old range contained a lock, then die_mem_heap will complain.
         Is that the correct behaviour?  Not sure. */
      evh__die_mem_heap( payload, md->szB );

      /* Copy from old to new */
      for (i = 0; i < md->szB; i++)
         ((UChar*)p_new)[i] = ((UChar*)payload)[i];

      /* Because the metadata hash table is index by payload address,
         we have to get rid of the old hash table entry and make a new
         one.  We can't just modify the existing metadata in place,
         because then it would (almost certainly) be in the wrong hash
         chain. */
      md_new = new_MallocMeta();
      *md_new = *md;

      md_tmp = VG_(HT_remove)( hg_mallocmeta_table, payload );
      tl_assert(md_tmp);
      tl_assert(md_tmp == md);

      VG_(cli_free)((void*)md->payload);
      delete_MallocMeta(md);

      /* Update fields */
      md_new->where   = VG_(record_ExeContext)( tid, 0 );
      md_new->szB     = new_size;
      md_new->payload = p_new;
      md_new->thr     = map_threads_lookup( tid );

      /* and add */
      VG_(HT_add_node)( hg_mallocmeta_table, (VgHashNode*)md_new );

      return (void*)p_new;
   }  
}


/*--------------------------------------------------------------*/
/*--- Instrumentation                                        ---*/
/*--------------------------------------------------------------*/

static void instrument_mem_access ( IRSB*   bbOut, 
                                    IRExpr* addr,
                                    Int     szB,
                                    Bool    isStore,
                                    Int     hWordTy_szB,
                                    VexGuestLayout* layout )
{
   IRType   tyAddr   = Ity_INVALID;
   HChar*   hName    = NULL;
   void*    hAddr    = NULL;
   Int      regparms = 0;
   IRExpr** argv     = NULL;
   IRDirty* di       = NULL;
   IRTemp   sp;
   IRExpr*  spE;

   tl_assert(isIRAtom(addr));
   tl_assert(hWordTy_szB == 4 || hWordTy_szB == 8);

   tyAddr = typeOfIRExpr( bbOut->tyenv, addr );
   tl_assert(tyAddr == Ity_I32 || tyAddr == Ity_I64);

   /* Get the guest's stack pointer, so we can pass it to the helper.
      How do we know this is up to date?  Presumably because SP is
      flushed to guest state before every memory reference. */
   tl_assert(sizeof(void*) == layout->sizeof_SP);
   tl_assert(sizeof(void*) == hWordTy_szB);
   if (layout->sizeof_SP == 4) {
      sp = newIRTemp(bbOut->tyenv, Ity_I32);
      addStmtToIRSB(
         bbOut,
         IRStmt_WrTmp( sp, IRExpr_Get( layout->offset_SP, Ity_I32 ) )
      );
   } else {
      tl_assert(layout->sizeof_SP == 8);
      sp = newIRTemp(bbOut->tyenv, Ity_I64);
      addStmtToIRSB(
         bbOut,
         IRStmt_WrTmp( sp, IRExpr_Get( layout->offset_SP, Ity_I64 ) )
      );
   }
   spE = IRExpr_RdTmp( sp );

   /* So the effective address is in 'addr' now. */
   regparms = 2; // unless stated otherwise
   if (isStore) {
      switch (szB) {
         case 1:
            hName = "evh__mem_help_write_1";
            hAddr = &evh__mem_help_write_1;
            argv = mkIRExprVec_2( addr, spE );
            break;
         case 2:
            hName = "evh__mem_help_write_2";
            hAddr = &evh__mem_help_write_2;
            argv = mkIRExprVec_2( addr, spE );
            break;
         case 4:
            hName = "evh__mem_help_write_4";
            hAddr = &evh__mem_help_write_4;
            argv = mkIRExprVec_2( addr, spE );
            break;
         case 8:
            hName = "evh__mem_help_write_8";
            hAddr = &evh__mem_help_write_8;
            argv = mkIRExprVec_2( addr, spE );
            break;
         default:
            tl_assert(szB > 8 && szB <= 512); /* stay sane */
            regparms = 3;
            hName = "evh__mem_help_write_N";
            hAddr = &evh__mem_help_write_N;
            argv = mkIRExprVec_3( addr, mkIRExpr_HWord( szB ), spE);
            break;
      }
   } else {
      switch (szB) {
         case 1:
            hName = "evh__mem_help_read_1";
            hAddr = &evh__mem_help_read_1;
            argv = mkIRExprVec_2( addr, spE );
            break;
         case 2:
            hName = "evh__mem_help_read_2";
            hAddr = &evh__mem_help_read_2;
            argv = mkIRExprVec_2( addr, spE );
            break;
         case 4:
            hName = "evh__mem_help_read_4";
            hAddr = &evh__mem_help_read_4;
            argv = mkIRExprVec_2( addr, spE );
            break;
         case 8:
            hName = "evh__mem_help_read_8";
            hAddr = &evh__mem_help_read_8;
            argv = mkIRExprVec_2( addr, spE );
            break;
         default: 
            tl_assert(szB > 8 && szB <= 512); /* stay sane */
            regparms = 3;
            hName = "evh__mem_help_read_N";
            hAddr = &evh__mem_help_read_N;
            argv = mkIRExprVec_3( addr, mkIRExpr_HWord( szB ), spE);
            break;
      }
   }

   /* Add the helper. */
   tl_assert(hName);
   tl_assert(hAddr);
   tl_assert(argv);
   di = unsafeIRDirty_0_N( regparms,
                           hName, VG_(fnptr_to_fnentry)( hAddr ),
                           argv );
   addStmtToIRSB( bbOut, IRStmt_Dirty(di) );
}


static void instrument_memory_bus_event ( IRSB* bbOut, IRMBusEvent event )
{
   switch (event) {
      case Imbe_Fence:
         break; /* not interesting */
      case Imbe_BusLock:
      case Imbe_BusUnlock:
         addStmtToIRSB(
            bbOut,
            IRStmt_Dirty(
               unsafeIRDirty_0_N( 
                  0/*regparms*/, 
                  event == Imbe_BusLock ? "evh__bus_lock"
                                        : "evh__bus_unlock",
                  VG_(fnptr_to_fnentry)(
                     event == Imbe_BusLock ? &evh__bus_lock 
                                           : &evh__bus_unlock 
                  ),
                  mkIRExprVec_0() 
               )
            )
         );
         break;
      default:
         tl_assert(0);
   }
}


static
IRSB* hg_instrument ( VgCallbackClosure* closure,
                      IRSB* bbIn,
                      VexGuestLayout* layout,
                      VexGuestExtents* vge,
                      IRType gWordTy, IRType hWordTy )
{
   Int   i;
   IRSB* bbOut;

   if (gWordTy != hWordTy) {
      /* We don't currently support this case. */
      VG_(tool_panic)("host/guest word size mismatch");
   }

   /* Set up BB */
   bbOut           = emptyIRSB();
   bbOut->tyenv    = deepCopyIRTypeEnv(bbIn->tyenv);
   bbOut->next     = deepCopyIRExpr(bbIn->next);
   bbOut->jumpkind = bbIn->jumpkind;

   // Copy verbatim any IR preamble preceding the first IMark
   i = 0;
   while (i < bbIn->stmts_used && bbIn->stmts[i]->tag != Ist_IMark) {
      addStmtToIRSB( bbOut, bbIn->stmts[i] );
      i++;
   }

   for (/*use current i*/; i < bbIn->stmts_used; i++) {
      IRStmt* st = bbIn->stmts[i];
      tl_assert(st);
      tl_assert(isFlatIRStmt(st));
      switch (st->tag) {
         case Ist_NoOp:
         case Ist_AbiHint:
         case Ist_Put:
         case Ist_PutI:
         case Ist_IMark:
         case Ist_Exit:
            /* None of these can contain any memory references. */
            break;

         case Ist_MBE:
            instrument_memory_bus_event( bbOut, st->Ist.MBE.event );
            break;

         case Ist_Store:
            instrument_mem_access( 
               bbOut, 
               st->Ist.Store.addr, 
               sizeofIRType(typeOfIRExpr(bbIn->tyenv, st->Ist.Store.data)),
               True/*isStore*/,
               sizeofIRType(hWordTy),
               layout
            );
            break;

         case Ist_WrTmp: {
            IRExpr* data = st->Ist.WrTmp.data;
            if (data->tag == Iex_Load) {
               instrument_mem_access(
                  bbOut,
                  data->Iex.Load.addr,
                  sizeofIRType(data->Iex.Load.ty),
                  False/*!isStore*/,
                  sizeofIRType(hWordTy),
                  layout
               );
            }
            break;
         }

         case Ist_Dirty: {
            Int      dataSize;
            IRDirty* d = st->Ist.Dirty.details;
            if (d->mFx != Ifx_None) {
               /* This dirty helper accesses memory.  Collect the
                  details. */
               tl_assert(d->mAddr != NULL);
               tl_assert(d->mSize != 0);
               dataSize = d->mSize;
               if (d->mFx == Ifx_Read || d->mFx == Ifx_Modify) {
                  instrument_mem_access( 
                     bbOut, d->mAddr, dataSize, False/*!isStore*/,
                     sizeofIRType(hWordTy), layout
                  );
               }
               if (d->mFx == Ifx_Write || d->mFx == Ifx_Modify) {
                  instrument_mem_access( 
                     bbOut, d->mAddr, dataSize, True/*isStore*/,
                     sizeofIRType(hWordTy), layout
                  );
               }
            } else {
               tl_assert(d->mAddr == NULL);
               tl_assert(d->mSize == 0);
            }
            break;
         }

         default:
            tl_assert(0);

      } /* switch (st->tag) */

      addStmtToIRSB( bbOut, st );
   } /* iterate over bbIn->stmts */

   return bbOut;
}


/*----------------------------------------------------------------*/
/*--- Client requests                                          ---*/
/*----------------------------------------------------------------*/

/* Sheesh.  Yet another goddam finite map. */
static WordFM* map_pthread_t_to_Thread = NULL; /* pthread_t -> Thread* */

static void map_pthread_t_to_Thread_INIT ( void ) {
   if (UNLIKELY(map_pthread_t_to_Thread == NULL)) {
      map_pthread_t_to_Thread = HG_(newFM)( hg_zalloc, hg_free, NULL );
      tl_assert(map_pthread_t_to_Thread != NULL);
   }
}


static 
Bool hg_handle_client_request ( ThreadId tid, UWord* args, UWord* ret)
{
   if (!VG_IS_TOOL_USERREQ('H','G',args[0]))
      return False;

   /* Anything that gets past the above check is one of ours, so we
      should be able to handle it. */

   /* default, meaningless return value, unless otherwise set */
   *ret = 0;

   switch (args[0]) {

      /* --- --- User-visible client requests --- --- */

      case VG_USERREQ__HG_CLEAN_MEMORY:
         if (0) VG_(printf)("VG_USERREQ__HG_CLEAN_MEMORY(%p,%d)\n",
                            args[1], args[2]);
         /* Call die_mem to (expensively) tidy up properly, if there
            are any held locks etc in the area */
         if (args[2] > 0) { /* length */
            evh__die_mem(args[1], args[2]);
            /* and then set it to New */
            evh__new_mem(args[1], args[2]);
         }
         break;

      /* --- --- Client requests for Helgrind's use only --- --- */

      /* Some thread is telling us its pthread_t value.  Record the
         binding between that and the associated Thread*, so we can
         later find the Thread* again when notified of a join by the
         thread. */
      case _VG_USERREQ__HG_SET_MY_PTHREAD_T: {
         Thread* my_thr = NULL;
         if (0)
         VG_(printf)("SET_MY_PTHREAD_T (tid %d): pthread_t = %p\n", (Int)tid,
                     (void*)args[1]);
         map_pthread_t_to_Thread_INIT();
         my_thr = map_threads_maybe_lookup( tid );
         /* This assertion should hold because the map_threads (tid to
            Thread*) binding should have been made at the point of
            low-level creation of this thread, which should have
            happened prior to us getting this client request for it.
            That's because this client request is sent from
            client-world from the 'thread_wrapper' function, which
            only runs once the thread has been low-level created. */
         tl_assert(my_thr != NULL);
         /* So now we know that (pthread_t)args[1] is associated with
            (Thread*)my_thr.  Note that down. */
         if (0)
         VG_(printf)("XXXX: bind pthread_t %p to Thread* %p\n",
                     (void*)args[1], (void*)my_thr );
         HG_(addToFM)( map_pthread_t_to_Thread, (Word)args[1], (Word)my_thr );
         break;
      }

      case _VG_USERREQ__HG_PTH_API_ERROR: {
         Thread* my_thr = NULL;
         map_pthread_t_to_Thread_INIT();
         my_thr = map_threads_maybe_lookup( tid );
         tl_assert(my_thr); /* See justification above in SET_MY_PTHREAD_T */
         record_error_PthAPIerror( my_thr, (HChar*)args[1], 
                                           (Word)args[2], (HChar*)args[3] );
         break;
      }

      /* This thread (tid) has completed a join with the quitting
         thread whose pthread_t is in args[1]. */
      case _VG_USERREQ__HG_PTHREAD_JOIN_POST: {
         Thread* thr_q = NULL; /* quitter Thread* */
         Bool    found = False;
         if (0)
         VG_(printf)("NOTIFY_JOIN_COMPLETE (tid %d): quitter = %p\n", (Int)tid,
                     (void*)args[1]);
         map_pthread_t_to_Thread_INIT();
         found = HG_(lookupFM)( map_pthread_t_to_Thread, 
                                NULL, (Word*)&thr_q, (Word)args[1] );
          /* Can this fail?  It would mean that our pthread_join
             wrapper observed a successful join on args[1] yet that
             thread never existed (or at least, it never lodged an
             entry in the mapping (via SET_MY_PTHREAD_T)).  Which
             sounds like a bug in the threads library. */
         // FIXME: get rid of this assertion; handle properly
         tl_assert(found);
         if (found) {
            if (0)
            VG_(printf)(".................... quitter Thread* = %p\n", 
                        thr_q);
            evh__HG_PTHREAD_JOIN_POST( tid, thr_q );
         }
         break;
      }

      /* EXPOSITION only: by intercepting lock init events we can show
         the user where the lock was initialised, rather than only
         being able to show where it was first locked.  Intercepting
         lock initialisations is not necessary for the basic operation
         of the race checker. */
      case _VG_USERREQ__HG_PTHREAD_MUTEX_INIT_POST:
         evh__HG_PTHREAD_MUTEX_INIT_POST( tid, (void*)args[1], args[2] );
         break;

      case _VG_USERREQ__HG_PTHREAD_MUTEX_DESTROY_PRE:
         evh__HG_PTHREAD_MUTEX_DESTROY_PRE( tid, (void*)args[1] );
         break;

      case _VG_USERREQ__HG_PTHREAD_MUTEX_UNLOCK_PRE:   // pth_mx_t*
         evh__HG_PTHREAD_MUTEX_UNLOCK_PRE( tid, (void*)args[1] );
         break;

      case _VG_USERREQ__HG_PTHREAD_MUTEX_UNLOCK_POST:  // pth_mx_t*
         evh__HG_PTHREAD_MUTEX_UNLOCK_POST( tid, (void*)args[1] );
         break;

      case _VG_USERREQ__HG_PTHREAD_MUTEX_LOCK_PRE:     // pth_mx_t*, Word
         evh__HG_PTHREAD_MUTEX_LOCK_PRE( tid, (void*)args[1], args[2] );
         break;

      case _VG_USERREQ__HG_PTHREAD_MUTEX_LOCK_POST:    // pth_mx_t*
         evh__HG_PTHREAD_MUTEX_LOCK_POST( tid, (void*)args[1] );
         break;

      /* This thread is about to do pthread_cond_signal on the
         pthread_cond_t* in arg[1].  Ditto pthread_cond_broadcast. */
      case _VG_USERREQ__HG_PTHREAD_COND_SIGNAL_PRE:
      case _VG_USERREQ__HG_PTHREAD_COND_BROADCAST_PRE:
         evh__HG_PTHREAD_COND_SIGNAL_PRE( tid, (void*)args[1] );
         break;

      /* Entry into pthread_cond_wait, cond=arg[1], mutex=arg[2].
         Returns a flag indicating whether or not the mutex is believed to be
         valid for this operation. */
      case _VG_USERREQ__HG_PTHREAD_COND_WAIT_PRE: {
         Bool mutex_is_valid
            = evh__HG_PTHREAD_COND_WAIT_PRE( tid, (void*)args[1], 
                                                  (void*)args[2] );
         *ret = mutex_is_valid ? 1 : 0;
         break;
      }

      /* Thread successfully completed pthread_cond_wait, cond=arg[1],
         mutex=arg[2] */
      case _VG_USERREQ__HG_PTHREAD_COND_WAIT_POST:
         evh__HG_PTHREAD_COND_WAIT_POST( tid,
                                         (void*)args[1], (void*)args[2] );
         break;

      case _VG_USERREQ__HG_PTHREAD_RWLOCK_INIT_POST:
         evh__HG_PTHREAD_RWLOCK_INIT_POST( tid, (void*)args[1] );
         break;

      case _VG_USERREQ__HG_PTHREAD_RWLOCK_DESTROY_PRE:
         evh__HG_PTHREAD_RWLOCK_DESTROY_PRE( tid, (void*)args[1] );
         break;

      /* rwlock=arg[1], isW=arg[2], isTryLock=arg[3] */
      case _VG_USERREQ__HG_PTHREAD_RWLOCK_LOCK_PRE:
         evh__HG_PTHREAD_RWLOCK_LOCK_PRE( tid, (void*)args[1],
                                               args[2], args[3] );
         break;

      /* rwlock=arg[1], isW=arg[2] */
      case _VG_USERREQ__HG_PTHREAD_RWLOCK_LOCK_POST:
         evh__HG_PTHREAD_RWLOCK_LOCK_POST( tid, (void*)args[1], args[2] );
         break;

      case _VG_USERREQ__HG_PTHREAD_RWLOCK_UNLOCK_PRE:
         evh__HG_PTHREAD_RWLOCK_UNLOCK_PRE( tid, (void*)args[1] );
         break;

      case _VG_USERREQ__HG_PTHREAD_RWLOCK_UNLOCK_POST:
         evh__HG_PTHREAD_RWLOCK_UNLOCK_POST( tid, (void*)args[1] );
         break;

      case _VG_USERREQ__HG_POSIX_SEM_INIT_POST: /* sem_t*, unsigned long */
         evh__HG_POSIX_SEM_INIT_POST( tid, (void*)args[1], args[2] );
         break;

      case _VG_USERREQ__HG_POSIX_SEM_DESTROY_PRE: /* sem_t* */
         evh__HG_POSIX_SEM_DESTROY_PRE( tid, (void*)args[1] );
         break;

      case _VG_USERREQ__HG_POSIX_SEM_POST_PRE: /* sem_t* */
         evh__HG_POSIX_SEM_POST_PRE( tid, (void*)args[1] );
         break;

      case _VG_USERREQ__HG_POSIX_SEM_WAIT_POST: /* sem_t* */
         evh__HG_POSIX_SEM_WAIT_POST( tid, (void*)args[1] );
         break;

      case _VG_USERREQ__HG_GET_MY_SEGMENT: { // -> Segment*
         Thread*   thr;
         SegmentID segid;
         Segment*  seg;
         thr = map_threads_maybe_lookup( tid );
         tl_assert(thr); /* cannot fail */
         segid = thr->csegid;
         tl_assert(SEG_id_is_sane(segid));
         seg = SEG_get( segid );
         tl_assert(seg);
         *ret = (UWord)seg;
         break;
      }

      case _VG_USERREQ__HG_GET_THREAD_ID: { // -> Thread ID
         Thread*   thr;
         thr = map_threads_maybe_lookup( tid );
         tl_assert(thr); /* cannot fail */
         *ret = (UWord)thr->threadUID;
         break;
      }

      case _VG_USERREQ__HG_GET_SEGMENT_ID: { // -> SegmentID
         Thread*   thr;
         thr = map_threads_maybe_lookup( tid );
         tl_assert(thr); /* cannot fail */
         *ret = (UWord)thr->csegid;
         break;
      }

      case VG_USERREQ__HG_EXPECT_RACE: { // void*, char*, char *, int
        Addr   ptr   = (Addr)  args[1];
        HChar* descr = (HChar*)args[2];
        HChar* file  = (HChar*)args[3];
        Int    line  = (Int)   args[4];
        maybe_set_expected_error(ptr, descr, file, line, False);
        break;
      }

      case VG_USERREQ__HG_BENIGN_RACE: { // void*, char*, char *, int
        Addr   ptr   = (Addr)  args[1];
        HChar* descr = (HChar*)args[2];
        HChar* file  = (HChar*)args[3];
        Int    line  = (Int)   args[4];
        maybe_set_expected_error(ptr, descr, file, line, True);
        break;
      }

      case VG_USERREQ__HG_PCQ_CREATE: // void *
         pcq_create(args[1]);
         break;
      case VG_USERREQ__HG_PCQ_DESTROY: // void *
         pcq_destroy(args[1]);
         break;
      case VG_USERREQ__HG_PCQ_PUT: // void *
         pcq_put(tid, args[1]);
         break;
      case VG_USERREQ__HG_PCQ_GET: // void *
         pcq_get(tid, args[1]);
         break;

      case VG_USERREQ__HG_TRACE_MEM:  // void *
         mem_trace_on(args[1], tid);
         break;

      case VG_USERREQ__HG_MUTEX_IS_USED_AS_CONDVAR: // void *
         set_mu_is_cv(args[1], tid);
         break;
      
      // These two client requests are useful to mark a section of code 
      // were user wants helgrind to ignore all reads. 
      // For and example of such case, see test69. 
      case VG_USERREQ__HG_IGNORE_READS_BEGIN: {
         Thread *thr = map_threads_maybe_lookup( tid );
         tl_assert(thr); /* cannot fail */
         tl_assert(!thr->ignore_reads);
         thr->ignore_reads = True;
         break;
      }
      case VG_USERREQ__HG_IGNORE_READS_END: {
         Thread *thr = map_threads_maybe_lookup( tid );
         tl_assert(thr); /* cannot fail */
         tl_assert(thr->ignore_reads);
         thr->ignore_reads = False;
         break;
      }
      case VG_USERREQ__HG_PRINT_MEMORY_USAGE: {
         pp_memory_usage(args[1], "Annotation");        
         break;
      }
      case VG_USERREQ__HG_PRINT_STATS: {
         pp_stats("Annotation");        
         break;
      }
      case VG_USERREQ__HG_RESET_STATS: {
         hg_reset_stats();        
         break;
      }

      default:
         /* Unhandled Helgrind client request! */
        tl_assert2(0, "unhandled Helgrind client request!");
   }

   return True;
}


/*----------------------------------------------------------------*/
/*--- Error management                                         ---*/
/*----------------------------------------------------------------*/

/* maps (by value) strings to a copy of them in ARENA_TOOL */
static UWord stats__string_table_queries = 0;
static WordFM* string_table = NULL;
static Word string_table_cmp ( UWord s1, UWord s2 ) {
   return (Word)VG_(strcmp)( (HChar*)s1, (HChar*)s2 );
}
static HChar* string_table_strdup ( HChar* str ) {
   HChar* copy = NULL;
   stats__string_table_queries++;
   if (!str)
      str = "(null)";
   if (!string_table) {
      string_table = HG_(newFM)( hg_zalloc, hg_free, string_table_cmp );
      tl_assert(string_table);
   }
   if (HG_(lookupFM)( string_table,
                      NULL, (Word*)&copy, (Word)str )) {
      tl_assert(copy);
      if (0) VG_(printf)("string_table_strdup: %p -> %p\n", str, copy );
      return copy;
   } else {
      copy = VG_(strdup)(str);
      tl_assert(copy);
      HG_(addToFM)( string_table, (Word)copy, (Word)copy );
      return copy;
   }
}

/* maps from Lock .unique fields to LockP*s */
static UWord stats__ga_LockN_to_P_queries = 0;
static WordFM* yaWFM = NULL;
static Word lock_unique_cmp ( UWord lk1W, UWord lk2W )
{
   Lock* lk1 = (Lock*)lk1W;
   Lock* lk2 = (Lock*)lk2W;
   tl_assert( is_sane_LockNorP(lk1) );
   tl_assert( is_sane_LockNorP(lk2) );
   if (lk1->unique < lk2->unique) return -1;
   if (lk1->unique > lk2->unique) return 1;
   return 0;
}
static Lock* mk_LockP_from_LockN ( Lock* lkn )
{
   Lock* lkp = NULL;
   stats__ga_LockN_to_P_queries++;
   tl_assert( is_sane_LockN(lkn) );
   if (!yaWFM) {
      yaWFM = HG_(newFM)( hg_zalloc, hg_free, lock_unique_cmp );
      tl_assert(yaWFM);
   }
   if (!HG_(lookupFM)( yaWFM, NULL, (Word*)&lkp, (Word)lkn)) {
      lkp = hg_zalloc( sizeof(Lock) );
      *lkp = *lkn;
      lkp->admin = NULL;
      lkp->magic = LockP_MAGIC;
      /* Forget about the bag of lock holders - don't copy that.
         Also, acquired_at should be NULL whenever heldBy is empty,
         and vice versa. */
      lkp->heldW  = False;
      VG_(memset)( &lkp->heldBy, 0, sizeof(lkp->heldBy) );
      lkp->acquired_at = NULL;
      HG_(addToFM)( yaWFM, (Word)lkp, (Word)lkp );
   }
   tl_assert( is_sane_LockP(lkp) );
   return lkp;
}

/* Errors:

      race: program counter
            read or write
            data size
            previous state
            current state

      FIXME: how does state printing interact with lockset gc?
      Are the locksets in prev/curr state always valid?
      Ditto question for the threadsets
          ThreadSets - probably are always valid if Threads
          are never thrown away.
          LockSets - could at least print the lockset elements that
          correspond to actual locks at the time of printing.  Hmm.
*/

/* Error kinds */
typedef
   enum {
      XE_Race=1101,      // race
      XE_FreeMemLock,    // freeing memory containing a locked lock
      XE_UnlockUnlocked, // unlocking a not-locked lock
      XE_UnlockForeign,  // unlocking a lock held by some other thread
      XE_UnlockBogus,    // unlocking an address not known to be a lock
      XE_PthAPIerror,    // error from the POSIX pthreads API
      XE_LockOrder,      // lock order error
      XE_Misc            // misc other error (w/ string to describe it)
   }
   XErrorTag;

/* Extra contexts for kinds */
typedef
   struct  {
      XErrorTag tag;
      union {
         struct {
            Addr  data_addr;
            Int   szB;
            Bool  isWrite;
            SVal  new_state;
            SVal  old_state;
            ExeContext* mb_lastlock;
            Thread* thr;
            /* Describe addr in a variable, or data sym */
            HChar descr1[96];
            HChar descr2[96];
            /* Describe addr in a malloc'd block */
            SizeT       block_szB;
            OffT        block_rwoffset;
            ExeContext* block_allocdAt;
         } Race;
         struct {
            Thread* thr;  /* doing the freeing */
            Lock*   lock; /* lock which is locked */
         } FreeMemLock;
         struct {
            Thread* thr;  /* doing the unlocking */
            Lock*   lock; /* lock (that is already unlocked) */
         } UnlockUnlocked;
         struct {
            Thread* thr;    /* doing the unlocking */
            Thread* owner;  /* thread that actually holds the lock */
            Lock*   lock;   /* lock (that is held by 'owner') */
         } UnlockForeign;
         struct {
            Thread* thr;     /* doing the unlocking */
            Addr    lock_ga; /* purported address of the lock */
         } UnlockBogus;
         struct {
            Thread* thr; 
            HChar*  fnname; /* persistent, in tool-arena */
            Word    err;    /* pth error code */
            HChar*  errstr; /* persistent, in tool-arena */
         } PthAPIerror;
         struct {
            Thread*     thr;
            Addr        before_ga; /* always locked first in prog. history */
            Addr        after_ga;
            ExeContext* before_ec;
            ExeContext* after_ec;
         } LockOrder;
         struct {
            Thread* thr;
            HChar*  errstr; /* persistent, in tool-arena */
         } Misc;
      } XE;
   }
   XError;

static void init_XError ( XError* xe ) {
   VG_(memset)(xe, 0, sizeof(*xe) );
   xe->tag = XE_Race-1; /* bogus */
}


/* Extensions of suppressions */
typedef
   enum {
      XS_Race=1201, /* race */
      XS_FreeMemLock,
      XS_UnlockUnlocked,
      XS_UnlockForeign,
      XS_UnlockBogus,
      XS_PthAPIerror,
      XS_LockOrder,
      XS_Misc
   }
   XSuppTag;


/* Updates the copy with address info if necessary. */
static UInt hg_update_extra ( Error* err )
{
   XError* xe = (XError*)VG_(get_error_extra)(err);
   tl_assert(xe);

   if (xe->tag == XE_Race) {
      MallocMeta* md;

      tl_assert(sizeof(xe->XE.Race.descr1) == sizeof(xe->XE.Race.descr2));

      /* Perhaps in a malloc'd block? */
      xe->XE.Race.descr1[0] = xe->XE.Race.descr2[0] = 0;
      VG_(HT_ResetIter)(hg_mallocmeta_table);
      while ( (md = VG_(HT_Next)(hg_mallocmeta_table)) ) {
         if (md->payload <= xe->XE.Race.data_addr
             && xe->XE.Race.data_addr < md->payload + md->szB) {
            xe->XE.Race.block_szB
               = md->szB;
            xe->XE.Race.block_rwoffset
               = (Word)xe->XE.Race.data_addr - (Word)md->payload;
            xe->XE.Race.block_allocdAt
               = md->where;
            goto out;
         }
      }

      /* Perhaps the variable type/location data describes it? */
      xe->XE.Race.descr1[0] = xe->XE.Race.descr2[0] = 0;
      if (VG_(get_data_description)(
                &xe->XE.Race.descr1[0],
                &xe->XE.Race.descr2[0],
                sizeof(xe->XE.Race.descr1)-1,
                xe->XE.Race.data_addr )) {
         tl_assert( xe->XE.Race.descr1
                       [ sizeof(xe->XE.Race.descr1)-1 ] == 0);
         tl_assert( xe->XE.Race.descr2
                       [ sizeof(xe->XE.Race.descr2)-1 ] == 0);
         goto out;
      }

      /* No description.  Bummer. */
      xe->XE.Race.descr1[0] = xe->XE.Race.descr2[0] = 0;

   } /* if (xe->tag == XE_Race) */

  out:
   return sizeof(XError);
}

// Ugly. Need to return a value from apply_StackTrace()...
static Bool destructor_detected = False;
// A callback to be passed to apply_StackTrace(). 
// A function is a DTOR iff it contains '::~'.
static void detect_destructor(UInt n, Addr ip)
{
   static UChar buf[4096];
   VG_(describe_IP)(ip, buf, sizeof(buf));
   if (VG_(strstr)(buf, "::~")) {
      destructor_detected = True;
   }
}

static Bool record_error_Race ( Thread* thr, 
                                Addr data_addr, Bool isWrite, Int szB,
                                SVal old_sv, SVal new_sv,
                                ExeContext* mb_lastlock ) {
   XError xe;
   ThreadId tid = map_threads_maybe_reverse_lookup(thr);
   tl_assert( is_sane_Thread(thr) );
   init_XError(&xe);
   xe.tag = XE_Race;
   xe.XE.Race.data_addr      = data_addr;
   xe.XE.Race.szB            = szB;
   xe.XE.Race.isWrite        = isWrite;
   xe.XE.Race.new_state      = new_sv;
   xe.XE.Race.old_state      = old_sv;
   xe.XE.Race.mb_lastlock    = mb_lastlock;
   xe.XE.Race.thr            = thr;
   xe.XE.Race.descr1[0]      = 0;
   xe.XE.Race.descr2[0]      = 0;
   xe.XE.Race.block_szB      = 0;
   xe.XE.Race.block_rwoffset = 0;
   xe.XE.Race.block_allocdAt = NULL;

   // FIXME: tid vs thr
   tl_assert(isWrite == False || isWrite == True);
   tl_assert(szB == 8 || szB == 4 || szB == 2 || szB == 1);

   /* If the PC is in something that looks like a PLT section, ignore
      any reported races.  It appears that ld.so does intentionally
      racey things in PLTs and it's simplest just to ignore it. */
   if (1) {
      if (tid != VG_INVALID_THREADID) {
         Addr ip_at_error = VG_(get_IP)( tid );
         if (VG_(seginfo_sect_kind)(NULL, 0, ip_at_error) == Vg_SectPLT) {
            /* ignore this race. */
            return False;
         }
      }
   }

   /* Do not collect this error if it is expected or benign */
   if (1) {
      ExpectedError *expected_error = get_expected_error((Word)data_addr);
      if (expected_error) {
         expected_error->detected = True;
         return False;
      }
   }

   destructor_detected = False;
   if (1) {
      // check if the stack trace contains a DTOR
      ExeContext *context = VG_(record_ExeContext)(tid,-1/*first_ip_delta*/);
      VG_(apply_ExeContext)(detect_destructor, context, 1000);
      if (destructor_detected && clo_ignore_in_dtor) return False;
   }

   Bool res = VG_(maybe_record_error)( map_threads_reverse_lookup(thr),
                            XE_Race, data_addr, NULL, &xe );

   if (res && destructor_detected) {
      VG_(message)(Vg_UserMsg, "NOTE: this race was detected inside a DTOR");
   }
   return res;
}

static void record_error_FreeMemLock ( Thread* thr, Lock* lk ) {
   XError xe;
   tl_assert( is_sane_Thread(thr) );
   tl_assert( is_sane_LockN(lk) );
   init_XError(&xe);
   xe.tag = XE_FreeMemLock;
   xe.XE.FreeMemLock.thr  = thr;
   xe.XE.FreeMemLock.lock = mk_LockP_from_LockN(lk);
   // FIXME: tid vs thr
   VG_(maybe_record_error)( map_threads_reverse_lookup(thr),
                            XE_FreeMemLock, 0, NULL, &xe );
}

static void record_error_UnlockUnlocked ( Thread* thr, Lock* lk ) {
   XError xe;
   tl_assert( is_sane_Thread(thr) );
   tl_assert( is_sane_LockN(lk) );
   init_XError(&xe);
   xe.tag = XE_UnlockUnlocked;
   xe.XE.UnlockUnlocked.thr  = thr;
   xe.XE.UnlockUnlocked.lock = mk_LockP_from_LockN(lk);
   // FIXME: tid vs thr
   VG_(maybe_record_error)( map_threads_reverse_lookup(thr),
                            XE_UnlockUnlocked, 0, NULL, &xe );
}

static void record_error_UnlockForeign ( Thread* thr,
                                         Thread* owner, Lock* lk ) {
   XError xe;
   tl_assert( is_sane_Thread(thr) );
   tl_assert( is_sane_Thread(owner) );
   tl_assert( is_sane_LockN(lk) );
   init_XError(&xe);
   xe.tag = XE_UnlockForeign;
   xe.XE.UnlockForeign.thr   = thr;
   xe.XE.UnlockForeign.owner = owner;
   xe.XE.UnlockForeign.lock  = mk_LockP_from_LockN(lk);
   // FIXME: tid vs thr
   VG_(maybe_record_error)( map_threads_reverse_lookup(thr),
                            XE_UnlockForeign, 0, NULL, &xe );
}

static void record_error_UnlockBogus ( Thread* thr, Addr lock_ga ) {
   XError xe;
   tl_assert( is_sane_Thread(thr) );
   init_XError(&xe);
   xe.tag = XE_UnlockBogus;
   xe.XE.UnlockBogus.thr     = thr;
   xe.XE.UnlockBogus.lock_ga = lock_ga;
   // FIXME: tid vs thr
   VG_(maybe_record_error)( map_threads_reverse_lookup(thr),
                            XE_UnlockBogus, 0, NULL, &xe );
}

static 
void record_error_LockOrder ( Thread* thr, Addr before_ga, Addr after_ga,
                              ExeContext* before_ec, ExeContext* after_ec ) {
   XError xe;
   tl_assert( is_sane_Thread(thr) );
   init_XError(&xe);
   xe.tag = XE_LockOrder;
   xe.XE.LockOrder.thr       = thr;
   xe.XE.LockOrder.before_ga = before_ga;
   xe.XE.LockOrder.before_ec = before_ec;
   xe.XE.LockOrder.after_ga  = after_ga;
   xe.XE.LockOrder.after_ec  = after_ec;
   // FIXME: tid vs thr
   VG_(maybe_record_error)( map_threads_reverse_lookup(thr),
                            XE_LockOrder, 0, NULL, &xe );
}

static 
void record_error_PthAPIerror ( Thread* thr, HChar* fnname, 
                                Word err, HChar* errstr ) {
   XError xe;
   tl_assert( is_sane_Thread(thr) );
   tl_assert(fnname);
   tl_assert(errstr);
   init_XError(&xe);
   xe.tag = XE_PthAPIerror;
   xe.XE.PthAPIerror.thr    = thr;
   xe.XE.PthAPIerror.fnname = string_table_strdup(fnname);
   xe.XE.PthAPIerror.err    = err;
   xe.XE.PthAPIerror.errstr = string_table_strdup(errstr);
   // FIXME: tid vs thr
   VG_(maybe_record_error)( map_threads_reverse_lookup(thr),
                            XE_PthAPIerror, 0, NULL, &xe );
}

static void record_error_Misc ( Thread* thr, HChar* errstr ) {
   XError xe;
   tl_assert( is_sane_Thread(thr) );
   tl_assert(errstr);
   init_XError(&xe);
   xe.tag = XE_Misc;
   xe.XE.Misc.thr    = thr;
   xe.XE.Misc.errstr = string_table_strdup(errstr);
   // FIXME: tid vs thr
   VG_(maybe_record_error)( map_threads_reverse_lookup(thr),
                            XE_Misc, 0, NULL, &xe );
}

static Bool hg_eq_Error ( VgRes not_used, Error* e1, Error* e2 )
{
   XError *xe1, *xe2;

   tl_assert(VG_(get_error_kind)(e1) == VG_(get_error_kind)(e2));

   xe1 = (XError*)VG_(get_error_extra)(e1);
   xe2 = (XError*)VG_(get_error_extra)(e2);
   tl_assert(xe1);
   tl_assert(xe2);

   switch (VG_(get_error_kind)(e1)) {
      case XE_Race:
         return xe1->XE.Race.szB == xe2->XE.Race.szB
                && xe1->XE.Race.isWrite == xe2->XE.Race.isWrite
                && (clo_cmp_race_err_addrs 
                       ? xe1->XE.Race.data_addr == xe2->XE.Race.data_addr
                       : True);
      case XE_FreeMemLock:
         return xe1->XE.FreeMemLock.thr == xe2->XE.FreeMemLock.thr
                && xe1->XE.FreeMemLock.lock == xe2->XE.FreeMemLock.lock;
      case XE_UnlockUnlocked:
         return xe1->XE.UnlockUnlocked.thr == xe2->XE.UnlockUnlocked.thr
                && xe1->XE.UnlockUnlocked.lock == xe2->XE.UnlockUnlocked.lock;
      case XE_UnlockForeign:
         return xe1->XE.UnlockForeign.thr == xe2->XE.UnlockForeign.thr
                && xe1->XE.UnlockForeign.owner == xe2->XE.UnlockForeign.owner
                && xe1->XE.UnlockForeign.lock == xe2->XE.UnlockForeign.lock;
      case XE_UnlockBogus:
         return xe1->XE.UnlockBogus.thr == xe2->XE.UnlockBogus.thr
                && xe1->XE.UnlockBogus.lock_ga == xe2->XE.UnlockBogus.lock_ga;
      case XE_PthAPIerror:
         return xe1->XE.PthAPIerror.thr == xe2->XE.PthAPIerror.thr
                && 0==VG_(strcmp)(xe1->XE.PthAPIerror.fnname,
                                  xe2->XE.PthAPIerror.fnname)
                && xe1->XE.PthAPIerror.err == xe2->XE.PthAPIerror.err;
      case XE_LockOrder:
         return xe1->XE.LockOrder.thr == xe2->XE.LockOrder.thr;
      case XE_Misc:
         return xe1->XE.Misc.thr == xe2->XE.Misc.thr
                && 0==VG_(strcmp)(xe1->XE.Misc.errstr, xe2->XE.Misc.errstr);
      default:
         tl_assert(0);
   }

   /*NOTREACHED*/
   tl_assert(0);
}

static void announce_one_thread ( Thread* thr ) {
   tl_assert(is_sane_Thread(thr));
   tl_assert(thr->threadUID >= 1);
   if (thr->announced)
      return;
   if (thr->threadUID == 1/*FIXME: this hardwires an assumption
                               about the identity of the root
                               thread*/) {
      tl_assert(thr->created_at == NULL);
      VG_(message)(Vg_UserMsg, "Thread T%d is the program's root thread",
                   thr->threadUID);
   } else {
      tl_assert(thr->created_at != NULL);
      VG_(message)(Vg_UserMsg, "Thread T%d was created",
                   thr->threadUID);
      VG_(pp_ExeContext)( thr->created_at );
   }
   VG_(message)(Vg_UserMsg, "");
   thr->announced = True;

}

static int add_threads_from_sval_to_array (SVal sv, XArray *xa) 
{
   UWord i;
   SegmentSet SS;
   if (!is_SHVAL_RM(sv)) return 0;
   SS = get_SHVAL_SS(sv);
   for (i = 0; i < SS_get_size(SS); i++) {
      SegmentID segid = SS_get_element(SS, i);
      Thread *thr = SEG_get(segid)->thr;
      tl_assert(is_sane_Thread(thr));
      VG_(addToXA)( xa, (void*)&thr );
   }
   return i;
}

/* Given a WordSetID in univ_tsets (that is, a Thread set ID), produce
   an XArray* with the corresponding Thread*'s sorted by their
   threadUID fields.  This is for printing out thread sets in
   repeatable orders, which is important for for repeatable regression
   testing.  The returned XArray* is dynamically allocated (of course)
   and so must be hg_freed by the caller. */
static Int cmp_Thread_by_errmsg_index ( void* thr1V, void* thr2V ) {
   Thread* thr1 = *(Thread**)thr1V;
   Thread* thr2 = *(Thread**)thr2V;
   if (thr1->threadUID < thr2->threadUID) return -1;
   if (thr1->threadUID > thr2->threadUID) return  1;
   return 0;
}


static void hg_pp_Error ( Error* err )
{
   XError *xe = (XError*)VG_(get_error_extra)(err);

   switch (VG_(get_error_kind)(err)) {

   case XE_Misc: {
      tl_assert(xe);
      tl_assert( is_sane_Thread( xe->XE.Misc.thr ) );
      announce_one_thread( xe->XE.Misc.thr );
      VG_(message)(Vg_UserMsg,
                  "Thread #%d: %s",
                  (Int)xe->XE.Misc.thr->threadUID,
                  xe->XE.Misc.errstr);
      VG_(pp_ExeContext)( VG_(get_error_where)(err) );
      break;
   }

   case XE_LockOrder: {
      tl_assert(xe);
      tl_assert( is_sane_Thread( xe->XE.LockOrder.thr ) );
      announce_one_thread( xe->XE.LockOrder.thr );
      VG_(message)(Vg_UserMsg,
                  "Thread #%d: lock order \"%p before %p\" violated",
                  (Int)xe->XE.LockOrder.thr->threadUID,
                  (void*)xe->XE.LockOrder.before_ga,
                  (void*)xe->XE.LockOrder.after_ga);
      VG_(pp_ExeContext)( VG_(get_error_where)(err) );
      if (xe->XE.LockOrder.before_ec && xe->XE.LockOrder.after_ec) {
         VG_(message)(Vg_UserMsg,
            "  Required order was established by acquisition of lock at %p",
            (void*)xe->XE.LockOrder.before_ga);
         VG_(pp_ExeContext)( xe->XE.LockOrder.before_ec );
         VG_(message)(Vg_UserMsg,
            "  followed by a later acquisition of lock at %p", 
            (void*)xe->XE.LockOrder.after_ga);
         VG_(pp_ExeContext)( xe->XE.LockOrder.after_ec );
      }
      break;
   }

   case XE_PthAPIerror: {
      tl_assert(xe);
      tl_assert( is_sane_Thread( xe->XE.PthAPIerror.thr ) );
      announce_one_thread( xe->XE.PthAPIerror.thr );
      VG_(message)(Vg_UserMsg,
                  "Thread #%d's call to %s failed",
                  (Int)xe->XE.PthAPIerror.thr->threadUID,
                  xe->XE.PthAPIerror.fnname);
      VG_(message)(Vg_UserMsg,
                  "   with error code %ld (%s)",
                  xe->XE.PthAPIerror.err,
                  xe->XE.PthAPIerror.errstr);
      VG_(pp_ExeContext)( VG_(get_error_where)(err) );
      break;
   }

   case XE_UnlockBogus: {
      tl_assert(xe);
      tl_assert( is_sane_Thread( xe->XE.UnlockBogus.thr ) );
      announce_one_thread( xe->XE.UnlockBogus.thr );
      VG_(message)(Vg_UserMsg,
                   "Thread #%d unlocked an invalid lock at %p ",
                   (Int)xe->XE.UnlockBogus.thr->threadUID,
                   (void*)xe->XE.UnlockBogus.lock_ga);
      VG_(pp_ExeContext)( VG_(get_error_where)(err) );
      break;
   }

   case XE_UnlockForeign: {
      tl_assert(xe);
      tl_assert( is_sane_LockP( xe->XE.UnlockForeign.lock ) );
      tl_assert( is_sane_Thread( xe->XE.UnlockForeign.owner ) );
      tl_assert( is_sane_Thread( xe->XE.UnlockForeign.thr ) );
      announce_one_thread( xe->XE.UnlockForeign.thr );
      announce_one_thread( xe->XE.UnlockForeign.owner );
      VG_(message)(Vg_UserMsg,
                   "Thread #%d unlocked lock at %p "
                   "currently held by thread #%d",
                   (Int)xe->XE.UnlockForeign.thr->threadUID,
                   (void*)xe->XE.UnlockForeign.lock->guestaddr,
                   (Int)xe->XE.UnlockForeign.owner->threadUID );
      VG_(pp_ExeContext)( VG_(get_error_where)(err) );
      if (xe->XE.UnlockForeign.lock->appeared_at) {
         VG_(message)(Vg_UserMsg,
                      "  Lock at %p was first observed",
                      (void*)xe->XE.UnlockForeign.lock->guestaddr);
         VG_(pp_ExeContext)( xe->XE.UnlockForeign.lock->appeared_at );
      }
      break;
   }

   case XE_UnlockUnlocked: {
      tl_assert(xe);
      tl_assert( is_sane_LockP( xe->XE.UnlockUnlocked.lock ) );
      tl_assert( is_sane_Thread( xe->XE.UnlockUnlocked.thr ) );
      announce_one_thread( xe->XE.UnlockUnlocked.thr );
      VG_(message)(Vg_UserMsg,
                   "Thread #%d unlocked a not-locked lock at %p ",
                   (Int)xe->XE.UnlockUnlocked.thr->threadUID,
                   (void*)xe->XE.UnlockUnlocked.lock->guestaddr);
      VG_(pp_ExeContext)( VG_(get_error_where)(err) );
      if (xe->XE.UnlockUnlocked.lock->appeared_at) {
         VG_(message)(Vg_UserMsg,
                      "  Lock at %p was first observed",
                      (void*)xe->XE.UnlockUnlocked.lock->guestaddr);
         VG_(pp_ExeContext)( xe->XE.UnlockUnlocked.lock->appeared_at );
      }
      break;
   }

   case XE_FreeMemLock: {
      tl_assert(xe);
      tl_assert( is_sane_LockP( xe->XE.FreeMemLock.lock ) );
      tl_assert( is_sane_Thread( xe->XE.FreeMemLock.thr ) );
      announce_one_thread( xe->XE.FreeMemLock.thr );
      VG_(message)(Vg_UserMsg,
                   "Thread #%d deallocated location %p "
                   "containing a locked lock",
                   (Int)xe->XE.FreeMemLock.thr->threadUID,
                   (void*)xe->XE.FreeMemLock.lock->guestaddr);
      VG_(pp_ExeContext)( VG_(get_error_where)(err) );
      if (xe->XE.FreeMemLock.lock->appeared_at) {
         VG_(message)(Vg_UserMsg,
                      "  Lock at %p was first observed",
                      (void*)xe->XE.FreeMemLock.lock->guestaddr);
         VG_(pp_ExeContext)( xe->XE.FreeMemLock.lock->appeared_at );
      }
      break;
   }

   case XE_Race: {
      Addr      err_ga;
      Char      old_buf[100], new_buf[100];
      SVal      old_state, new_state;
      Thread*   thr_acc;
      HChar*    what;
      Int       szB;
      XArray*   thread_xa;
      Int       i;

      /* First extract some essential info */
      tl_assert(xe);
      old_state = xe->XE.Race.old_state;
      new_state = xe->XE.Race.new_state;
      thr_acc   = xe->XE.Race.thr;
      what      = xe->XE.Race.isWrite ? "write" : "read";
      szB       = xe->XE.Race.szB;
      tl_assert(is_sane_Thread(thr_acc));
      err_ga = VG_(get_error_address)(err);

      /* Format the low level state print descriptions */
      show_sval(old_buf, sizeof(old_buf), old_state);
      show_sval(new_buf, sizeof(new_buf), new_state);
      
      // Announce the threads in order (sorted by threadUID). 
      // This is required to simplify regression testing. 
      thread_xa = VG_(newXA)( hg_zalloc, hg_free, sizeof(Thread*) );
      tl_assert(thread_xa);
      add_threads_from_sval_to_array(old_state, thread_xa);
      add_threads_from_sval_to_array(new_state, thread_xa);
      VG_(setCmpFnXA)( thread_xa, cmp_Thread_by_errmsg_index );
      VG_(sortXA)( thread_xa );
      for (i = 0; i < VG_(sizeXA)(thread_xa) && i < N_THREADS_TO_ANNOUNCE;
           i++) {
         Thread *thr = *(Thread**)VG_(indexXA)(thread_xa, i);
         announce_one_thread(thr);
      }
      VG_(deleteXA)(thread_xa);

      VG_(message)(Vg_UserMsg,
                   "T%d: Possible data race during %s of size %d at %p",
                   thr_acc->threadUID, 
                   what, szB, err_ga);
      VG_(pp_ExeContext)( VG_(get_error_where)(err) );
      VG_(message)(Vg_UserMsg, "  old state = {%s}", old_buf);
      VG_(message)(Vg_UserMsg, "  new state = {%s}", new_buf);

      if (is_SHVAL_RM(old_state)) {
         SegmentSet SS = get_SHVAL_SS(old_state);
         for (i = 0; i < (int)SS_get_size(SS); i++) {
            SegmentID segid = SS_get_element(SS, i);
            ExeContext *context = SEG_get_context(segid);
            if (context) {
               VG_(message)(Vg_UserMsg, " T%d/S%d starts", 
                            (Int)SEG_get(segid)->thr->threadUID,
                            (Int)segid);
               VG_(pp_ExeContext)(context);
            }
         }
      }


      // KCC:
      // The output looks like: 
      //==8746== Thread #1 is the program's root thread
      //==8746==
      //==8746== Thread #2 was created
      //==8746==    at 0x30002B0F41: clone (in /lib64/tls/libc-2.3.5.so)
      //          ...
      //==8746==    by 0x408528: main (racecheck_unittest.cc:140)
      //==8746==
      //==8746== T1: Possible data race during write of size 4 at 0x615158
      //==8746==    at 0x404928: test01::Parent() (racecheck_unittest.cc:217)
      //==8746==    by 0x4049A8: test01::Run() (racecheck_unittest.cc:223)
      //==8746==    by 0x408528: main (racecheck_unittest.cc:140)
      //==8746== old state: e000002000000000 W #SS=1 #LS=0 S2/T2
      //==8746== new state: c000001000000000 W #SS=2 #LS=0 S2/T2 S3/T1
      //
      // You might want to (ask me to) rewrite some of this, but...
      //
      // For me, the information printed above is enough. 
      // But probably because I know how the machine works :). 
      //
      // I find the information about 'last consistently used lock' 
      // misleading because in most cases that 'last' lock was held 
      // to protect something else. 
      //
      // Regarding reproducibility of output (for regression testing): 
      // The announced threads still go in order. 
      // But the order of segments listed in lines like 
      //   new state: c000001000000000 W #SS=2 #LS=0 S2/T2 S3/T1
      // depends on scheduler (segments are sorted by segment id). 
      // So we may want to filter out part of this string (after #LS=N).
      //
      // END KCC:

#if 0 // KCC: imho, we don't need all these variations. 
      /* CASE of Excl -> ShM */
      if (is_SHVAL_Excl(old_state) && is_SHVAL_ShM(new_state)) {
         SegmentID old_segid;
         Segment*  old_seg;
         Thread*   old_thr; 
         WordSetID new_tset;
         old_segid = un_SHVAL_Excl( old_state );
         tl_assert(SEG_id_is_sane(old_segid));
         old_seg = SEG_get( old_segid );
         tl_assert(is_sane_Segment(old_seg));
         tl_assert(old_seg->thr);
         old_thr = old_seg->thr;
         tl_assert(is_sane_Thread(old_thr));

         new_tset = un_SHVAL_ShM_tset(new_state);
         tset_to_announce = HG_(addToWS)( univ_tsets,
                                          new_tset, (Word)old_thr );
         announce_threadset( tset_to_announce );

         VG_(message)(Vg_UserMsg,
                      "Possible data race during %s of size %d at %p",
                      what, szB, err_ga);
         VG_(pp_ExeContext)( VG_(get_error_where)(err) );
         /* pp_AddrInfo(err_addr, &extra->addrinfo); */
         if (show_raw_states)
         VG_(message)(Vg_UserMsg,
                      "  Old state 0x%08x=%s, new state 0x%08x=%s",
                      old_state, old_buf, new_state, new_buf);
         VG_(message)(Vg_UserMsg,
                      "  Old state: owned exclusively by thread #%d",
                      old_thr->threadUID);
         // This should always show exactly 2 threads
         summarise_threadset( new_tset, new_tset_buf, sizeof(new_tset_buf) );
         VG_(message)(Vg_UserMsg,
                      "  New state: shared-modified by threads %s",
                      new_tset_buf );
         VG_(message)(Vg_UserMsg,
                      "  Reason:    this thread, #%d, holds no locks at all",
                      thr_acc->threadUID);
      }
      else 
      /* Case of ShR/M -> ShM */
      if (is_SHVAL_Sh(old_state) && is_SHVAL_ShM(new_state)) {
         WordSetID old_tset = un_SHVAL_Sh_tset(old_state);
         WordSetID new_tset = un_SHVAL_Sh_tset(new_state);

         tset_to_announce = HG_(unionWS)( univ_tsets, old_tset, new_tset );
         announce_threadset( tset_to_announce );

         VG_(message)(Vg_UserMsg,
                      "Possible data race during %s of size %d at %p",
                      what, szB, err_ga);
         VG_(pp_ExeContext)( VG_(get_error_where)(err) );
         /* pp_AddrInfo(err_addr, &extra->addrinfo); */
         if (show_raw_states)
         VG_(message)(Vg_UserMsg,
                      "  Old state 0x%08x=%s, new state 0x%08x=%s",
                      old_state, old_buf, new_state, new_buf);

         summarise_threadset( old_tset, old_tset_buf, sizeof(old_tset_buf) );
         summarise_threadset( new_tset, new_tset_buf, sizeof(new_tset_buf) );

         VG_(message)(Vg_UserMsg,
                      "  Old state: shared-%s by threads %s", 
                      is_SHVAL_ShM(old_state) ? "modified" : "readonly", 
                      old_tset_buf);
         VG_(message)(Vg_UserMsg,
                      "  New state: shared-modified by threads %s", 
                      new_tset_buf);
         VG_(message)(Vg_UserMsg,
                      "  Reason:    this thread, #%d, holds no "
                      "consistent locks",
                      thr_acc->threadUID);
         if (xe->XE.Race.mb_lastlock) {
            VG_(message)(Vg_UserMsg, "  Last consistently used lock for %p was "
                                     "first observed", err_ga);
            VG_(pp_ExeContext)(xe->XE.Race.mb_lastlock);
         } else {
            VG_(message)(Vg_UserMsg, "  Location %p has never been protected "
                                     "by any lock", err_ga);
         }
      }
      /* Hmm, unknown transition.  Just print what we do know. */
      else {
         VG_(message)(Vg_UserMsg,
                      "Possible data race during %s of size %d at %p",
                      what, szB, err_ga);
         VG_(pp_ExeContext)( VG_(get_error_where)(err) );

         //pp_AddrInfo(err_addr, &extra->addrinfo);
         VG_(message)(Vg_UserMsg,
                      "  Old state 0x%08x=%s, new state 0x%08x=%s",
                      old_state, old_buf, new_state, new_buf);
      }
#endif /* 0 */
      /* show the last lock lossage info */
      if (xe->XE.Race.mb_lastlock) {
         VG_(message)(Vg_UserMsg, "  Last consistently used lock for %p was "
                                  "first observed", err_ga);
         VG_(pp_ExeContext)(xe->XE.Race.mb_lastlock);
      } else {
         VG_(message)(Vg_UserMsg, "  Location 0x%lX has never been protected "
                                  "by any lock", err_ga);
      }

      /* If we have a better description of the address, show it. */
      if (xe->XE.Race.descr1[0] != 0)
         VG_(message)(Vg_UserMsg, "  %s", &xe->XE.Race.descr1);
      if (xe->XE.Race.descr2[0] != 0)
         VG_(message)(Vg_UserMsg, "  %s", &xe->XE.Race.descr2);
      if (xe->XE.Race.block_allocdAt) {
         VG_(message)(Vg_UserMsg,
            "  Location 0x%lX is %,ld bytes inside a block of "
            "size %,lu alloc'd",
            xe->XE.Race.data_addr, 
            xe->XE.Race.block_rwoffset, xe->XE.Race.block_szB
         );
         VG_(pp_ExeContext)( xe->XE.Race.block_allocdAt );
      }
      // print an empty line after the race report 
      VG_(message)(Vg_UserMsg, "");

      break; /* case XE_Race */
   } /* case XE_Race */

   default:
      tl_assert(0);
   } /* switch (VG_(get_error_kind)(err)) */
}

static Char* hg_get_error_name ( Error* err )
{
   switch (VG_(get_error_kind)(err)) {
      case XE_Race:           return "Race";
      case XE_FreeMemLock:    return "FreeMemLock";
      case XE_UnlockUnlocked: return "UnlockUnlocked";
      case XE_UnlockForeign:  return "UnlockForeign";
      case XE_UnlockBogus:    return "UnlockBogus";
      case XE_PthAPIerror:    return "PthAPIerror";
      case XE_LockOrder:      return "LockOrder";
      case XE_Misc:           return "Misc";
      default: tl_assert(0); /* fill in missing case */
   }
}

static Bool hg_recognised_suppression ( Char* name, Supp *su )
{
#  define TRY(_name,_xskind)                   \
      if (0 == VG_(strcmp)(name, (_name))) {   \
         VG_(set_supp_kind)(su, (_xskind));    \
         return True;                          \
      }
   TRY("Race",           XS_Race);
   TRY("FreeMemLock",    XS_FreeMemLock);
   TRY("UnlockUnlocked", XS_UnlockUnlocked);
   TRY("UnlockForeign",  XS_UnlockForeign);
   TRY("UnlockBogus",    XS_UnlockBogus);
   TRY("PthAPIerror",    XS_PthAPIerror);
   TRY("LockOrder",      XS_LockOrder);
   TRY("Misc",           XS_Misc);
   return False;
#  undef TRY
}

static Bool hg_read_extra_suppression_info ( Int fd, Char* buf, Int nBuf,
                                             Supp* su )
{
   /* do nothing -- no extra suppression info present.  Return True to
      indicate nothing bad happened. */
   return True;
}

static Bool hg_error_matches_suppression ( Error* err, Supp* su )
{
   switch (VG_(get_supp_kind)(su)) {
   case XS_Race:           return VG_(get_error_kind)(err) == XE_Race;
   case XS_FreeMemLock:    return VG_(get_error_kind)(err) == XE_FreeMemLock;
   case XS_UnlockUnlocked: return VG_(get_error_kind)(err) == XE_UnlockUnlocked;
   case XS_UnlockForeign:  return VG_(get_error_kind)(err) == XE_UnlockForeign;
   case XS_UnlockBogus:    return VG_(get_error_kind)(err) == XE_UnlockBogus;
   case XS_PthAPIerror:    return VG_(get_error_kind)(err) == XE_PthAPIerror;
   case XS_LockOrder:      return VG_(get_error_kind)(err) == XE_LockOrder;
   case XS_Misc:           return VG_(get_error_kind)(err) == XE_Misc;
   //case XS_: return VG_(get_error_kind)(err) == XE_;
   default: tl_assert(0); /* fill in missing cases */
   }
}

static void hg_print_extra_suppression_info ( Error* err )
{
   /* Do nothing */
}


/*----------------------------------------------------------------*/
/*--- Setup                                                    ---*/
/*----------------------------------------------------------------*/

static Bool hg_process_cmd_line_option ( Char* arg )
{
   if      (VG_CLO_STREQ(arg, "--happens-before=none"))
      clo_happens_before = 0;
   else if (VG_CLO_STREQ(arg, "--happens-before=threads"))
      clo_happens_before = 1;
   else if (VG_CLO_STREQ(arg, "--happens-before=all"))
      clo_happens_before = 2;

   /* FIXME this is bad.  It assumes that --ignore-n= occurs before
      --ignore-i= on the command line. */
   else if (VG_CLO_STREQN(11, arg, "--ignore-n=")) {
      clo_ignore_n = VG_(atoll)(&arg[11]);
      tl_assert(clo_ignore_n == 0 || (clo_ignore_n > 0 
                                      && clo_ignore_i < clo_ignore_n));
   }
   else if (VG_CLO_STREQN(11, arg, "--ignore-i=")) {
      clo_ignore_i = VG_(atoll)(&arg[11]);
      tl_assert(clo_ignore_n == 0 || (clo_ignore_n > 0 
                                      && clo_ignore_i < clo_ignore_n));
   }
   else if (VG_CLO_STREQN(23, arg, "--max-segment-set-size=")) {
      clo_max_segment_set_size = VG_(atoll)(&arg[23]);
      if (clo_max_segment_set_size < 4)
         clo_max_segment_set_size = 4;
   }
   else if (VG_CLO_STREQN(19, arg, "--trace-after-race=")) {
      clo_trace_after_race = VG_(atoll)(&arg[19]);
   }
   else if (VG_CLO_STREQN(24, arg, "--num-callers-for-locks=")) {
      clo_num_callers_for_locks = VG_(atoll)(&arg[24]);
   }

   else if (VG_CLO_STREQ(arg, "--ss-recycle=yes"))
      clo_ss_recycle = True;
   else if (VG_CLO_STREQ(arg, "--ss-recycle=no"))
      clo_ss_recycle = False;
  
   else if (VG_CLO_STREQ(arg, "--more-context=yes"))
      clo_more_context = True;
   else if (VG_CLO_STREQ(arg, "--more-context=no"))
      clo_more_context = False;

   else if (VG_CLO_STREQ(arg, "--ignore-in-dtor=yes"))
      clo_ignore_in_dtor = True;
   else if (VG_CLO_STREQ(arg, "--ignore-in-dtor=no"))
      clo_ignore_in_dtor = False;

   else if (VG_CLO_STREQ(arg, "--pure-happens-before=yes"))
      clo_pure_happens_before = True;
   else if (VG_CLO_STREQ(arg, "--pure-happens-before=no"))
      clo_pure_happens_before = False;

   else if (VG_CLO_STREQ(arg, "--gen-vcg=no"))
      clo_gen_vcg = 0;
   else if (VG_CLO_STREQ(arg, "--gen-vcg=yes"))
      clo_gen_vcg = 1;
   else if (VG_CLO_STREQ(arg, "--gen-vcg=yes-w-vts"))
      clo_gen_vcg = 2;

   else if (VG_CLO_STREQ(arg, "--cmp-race-err-addrs=no"))
      clo_cmp_race_err_addrs = False;
   else if (VG_CLO_STREQ(arg, "--cmp-race-err-addrs=yes"))
      clo_cmp_race_err_addrs = True;

   else if (VG_CLO_STREQN(13, arg, "--trace-addr=")) {
      clo_trace_addr = VG_(atoll16)(&arg[13]);
      if (clo_trace_level == 0)
         clo_trace_level = 1;
   }
   else VG_BNUM_CLO(arg, "--trace-level", clo_trace_level, 0, 2)

   /* "stuvwx" --> stuvwx (binary) */
   else if (VG_CLO_STREQN(18, arg, "--hg-sanity-flags=")) {
      Int j;
      Char* opt = & arg[18];
   
      if (6 != VG_(strlen)(opt)) {
         VG_(message)(Vg_UserMsg, 
                      "--hg-sanity-flags argument must have 6 digits");
         return False;
      }
      for (j = 0; j < 6; j++) {
         if      ('0' == opt[j]) { /* do nothing */ }
         else if ('1' == opt[j]) clo_sanity_flags |= (1 << (6-1-j));
         else {
            VG_(message)(Vg_UserMsg, "--hg-sanity-flags argument can "
                                     "only contain 0s and 1s");
            return False;
         }
      }
      if (0) VG_(printf)("XXX sanity flags: 0x%x\n", clo_sanity_flags);
   }

   else 
      return VG_(replacement_malloc_process_cmd_line_option)(arg);

   return True;
}

static void hg_print_usage ( void )
{
   VG_(printf)(
"    --happens-before=none|threads|all   [all] consider no events, thread\n"
"      create/join, create/join/cvsignal/cvwait/semwait/post as sync points\n"
"    --trace-addr=0xXXYYZZ    show all state changes for address 0xXXYYZZ\n"
"    --trace-level=0|1|2      verbosity level of --trace-addr [1]\n"
"    --max-segment-set-size=<N>\n"
"                             limit mem use by limiting SegSet sizes [20]\n"
"    --ignore-n=<N>           speedup hack; add documentation\n"
"    --ignore-i=<N>           speedup hack; add documentation\n"
"    --ss-recycle=no|yes      recycle segment sets [yes]\n"
"    --pure-happens-before=no|yes\n"
"                             be a pure-happens-before detector  [no]\n"
"    --more-context=no|yes    record context at lock lossage\n"
"                             and at segment creation  [yes]\n"
"    --ignore-in-dtor=no|yes  suppress races involving C++ destructors [no]\n"
"    --trace-after-race=<N>   limits tracing of racey addresses  [50]\n"
"    --num-callers-for-locks=<N>  show <N> callers in stack traces\n"
"                                 for lock acquisitions/releases [9]\n"
   );
   VG_(replacement_malloc_print_usage)();
}

static void hg_print_debug_usage ( void )
{
   VG_(replacement_malloc_print_debug_usage)();
   VG_(printf)("    --gen-vcg=no|yes|yes-w-vts   show happens-before graph "
               "in .vcg format [no]\n");
   VG_(printf)("    --cmp-race-err-addrs=no|yes  are data addresses in "
               "race errors significant? [no]\n");
   VG_(printf)("    --hg-sanity-flags=<XXXXXX> sanity check "
               "  at events (X = 0|1) [000000]\n");
   VG_(printf)("    --hg-sanity-flags values:\n");
   VG_(printf)("       100000   crosscheck happens-before-graph searches\n");
   VG_(printf)("       010000   after changes to "
               "lock-order-acquisition-graph\n");
   VG_(printf)("       001000   at memory accesses (NB: not currently used)\n");
   VG_(printf)("       000100   at mem permission setting for "
               "ranges >= %d bytes\n", SCE_BIGRANGE_T);
   VG_(printf)("       000010   at lock/unlock events\n");
   VG_(printf)("       000001   at thread create/join events\n");
}

static void hg_post_clo_init ( void )
{
}

static void hg_reset_stats ( void )
{
   stats__cache_Z_fetches      = 0;
   stats__cache_Z_wbacks       = 0;
   stats__cache_F_fetches      = 0;
   stats__cache_F_wbacks       = 0;
   stats__cache_invals         = 0;
   stats__cache_flushes        = 0;
   stats__cache_totrefs        = 0;
   stats__cache_totmisses      = 0;
   stats__cache_make_New_arange = 0;
   stats__cache_make_New_inZrep = 0;
   stats__cline_normalises     = 0;
   stats__cline_read64s        = 0;
   stats__cline_read32s        = 0;
   stats__cline_read16s        = 0;
   stats__cline_read8s         = 0;
   stats__cline_write64s       = 0;
   stats__cline_write32s       = 0;
   stats__cline_write16s       = 0;
   stats__cline_write8s        = 0;
   stats__cline_set64s         = 0;
   stats__cline_set32s         = 0;
   stats__cline_set16s         = 0;
   stats__cline_set8s          = 0;
   stats__cline_get8s          = 0;
   stats__cline_copy8s         = 0;
   stats__cline_64to32splits   = 0;
   stats__cline_32to16splits   = 0;
   stats__cline_16to8splits    = 0;
   stats__cline_64to32pulldown = 0;
   stats__cline_32to16pulldown = 0;
   stats__cline_16to8pulldown  = 0;
   stats__ga_LL_adds           = 0;
   stats__ga_LockN_to_P_queries     = 0;
   stats__hbefore_gsearches    = 0;
   stats__hbefore_gsearchFs    = 0;
   stats__hbefore_hits         = 0;
   stats__hbefore_invals       = 0;
   stats__hbefore_queries      = 0;
   stats__hbefore_stk_hwm      = 0;
   stats__lockN_acquires       = 0;
   stats__lockN_releases       = 0;
   stats__msm_BHL_hack         = 0;
   stats__secmaps_search       = 0;
   stats__secmaps_search_slow  = 0;
   stats__secmaps_allocd       = 0;
   stats__secmap_ga_space_covered = 0;
   stats__secmap_linesZ_allocd = 0;
   stats__secmap_linesZ_bytes  = 0;
   stats__secmap_linesF_allocd = 0;
   stats__secmap_linesF_bytes  = 0;
   stats__secmap_iterator_steppings = 0;
   stats__mk_Segment       = 0;
   stats__fake_VTS_bytes_trashed = 0;
   stats__msm_Ignore       = 0;
   stats__msm_R_to_R       = 0;
   stats__msm_R_to_M       = 0;
   stats__msm_M_to_R       = 0;
   stats__msm_M_to_M       = 0;
   stats__msm_New_to_M     = 0;
   stats__msm_New_to_R     = 0;
   stats__msm_oldSS_single = 0;
   stats__msm_oldSS_multi  = 0;
   stats__msm_oldSS_multi_shortcut  = 0;
   stats__msm_oldSS_multi_add       = 0;
   stats__msm_oldSS_multi_del       = 0;
   stats__sanity_checks    = 0;
}

static void pp_stats ( Char * caller )
{
   VG_(printf)("Overall helgrind statistics (caller = \"%s\") {\n", caller);
      
   if (1) {
      HG_(ppWSUstats)( univ_ssets, "univ_ssets" );
      VG_(printf)("\n");
      HG_(ppWSUstats)( univ_lsets, "univ_lsets" );
      VG_(printf)("\n");
      HG_(ppWSUstats)( univ_laog,  "univ_laog" );
      VG_(printf)("\n");
   }

   VG_(printf)(" hbefore: %,10lu queries\n",        stats__hbefore_queries);
   VG_(printf)(" hbefore: %,10lu hash table hits\n",   stats__hbefore_hits);
   VG_(printf)(" hbefore: %,10lu graph searches\n", stats__hbefore_gsearches);
   
   // TODO: does this make any sense ?
   VG_(printf)(" hbefore: %,10lu   of which slow\n",
               stats__hbefore_gsearches - stats__hbefore_gsearchFs);
   
   VG_(printf)(" hbefore: %,10lu stack high water mark\n",
               stats__hbefore_stk_hwm);
   VG_(printf)(" hbefore: %,10lu cache invals\n",   stats__hbefore_invals);

   VG_(printf)("\n");
   VG_(printf)("        segments: %,11lu Segment objects allocated\n", 
               stats__mk_Segment);
   VG_(printf)("        locksets: %,11ld unique lock sets\n",
               (Word)HG_(cardinalityWSU)( univ_lsets ));
   VG_(printf)("     segmentsets: %,11ld unique segment sets\n",
               (Word)HG_(cardinalityWSU)( univ_ssets ));
   VG_(printf)("       univ_laog: %,11ld unique lock sets\n",
               (Word)HG_(cardinalityWSU)( univ_laog ));

   VG_(printf)("L(ast)L(ock) map: %,11lu inserts (%,ld map size)\n", 
               stats__ga_LL_adds,
               (Word)(ga_to_lastlock ? HG_(sizeFM)( ga_to_lastlock ) : 0) );

   VG_(printf)("  LockN-to-P map: %,11lu queries (%,ld map size)\n", 
               stats__ga_LockN_to_P_queries,
               (Word)(yaWFM ? HG_(sizeFM)( yaWFM ) : 0) );

   VG_(printf)("string table map: %,11lu queries (%,ld map size)\n", 
               stats__string_table_queries,
               (Word)(string_table ? HG_(sizeFM)( string_table ) : 0) );
   VG_(printf)("            LAOG: %,11ld map size\n", 
               (Word)(laog ? HG_(sizeFM)( laog ) : 0));
   VG_(printf)(" LAOG exposition: %,11ld map size\n", 
               (Word)(laog_exposition ? HG_(sizeFM)( laog_exposition ) : 0));
   VG_(printf)("           locks: %,11lu acquires, "
               "%,lu releases\n",
               stats__lockN_acquires,
               stats__lockN_releases
              );
   VG_(printf)("   sanity checks: %,11lu\n", stats__sanity_checks);
   VG_(printf)("     zalloc/free: %,11lu zallocs, %,lu frees\n", 
               stat__hg_zalloc, stat__hg_free);

   VG_(printf)("\n");
   VG_(printf)("     msm: %,14lu %,14lu  BHL-skipped, Ignore\n",
               stats__msm_BHL_hack, stats__msm_Ignore);
   VG_(printf)("     msm: %,14lu %,14lu  R_to_R,   R_to_M\n",
               stats__msm_R_to_R, stats__msm_R_to_M);
   VG_(printf)("     msm: %,14lu %,14lu  M_to_R,   M_to_M\n",
               stats__msm_M_to_R, stats__msm_M_to_M);
   VG_(printf)("     msm: %,14lu %,14lu  New_to_R, New_to_M\n",
               stats__msm_New_to_R, stats__msm_New_to_M);
   VG_(printf)("     msm: %,14lu                 SS_update_single\n", 
               stats__msm_oldSS_single);
   VG_(printf)("     msm: %,14lu %,14lu  SS_update_multi, shortcut\n", 
               stats__msm_oldSS_multi, stats__msm_oldSS_multi_shortcut);
   VG_(printf)("     msm: %,14lu %,14lu  SS_update_add, SS_update_del\n", 
               stats__msm_oldSS_multi_add, stats__msm_oldSS_multi_del);

   VG_(printf)("\n");
   VG_(printf)(" secmaps: %,10lu allocd (%,12lu g-a-range)\n",
               stats__secmaps_allocd,
               stats__secmap_ga_space_covered);
   VG_(printf)("  linesZ: %,10lu allocd (%,12lu bytes occupied)\n",
               stats__secmap_linesZ_allocd,
               stats__secmap_linesZ_bytes);
   VG_(printf)("  linesF: %,10lu allocd (%,12lu bytes occupied)\n",
               stats__secmap_linesF_allocd,
               stats__secmap_linesF_bytes);
   VG_(printf)(" secmaps: %,10lu iterator steppings\n",
               stats__secmap_iterator_steppings);
   VG_(printf)(" secmaps: %,10lu searches (%,12lu slow)\n",
               stats__secmaps_search, stats__secmaps_search_slow);

   VG_(printf)("\n");
   VG_(printf)("   cache: %,14lu totrefs (%,lu misses)\n",
               stats__cache_totrefs, stats__cache_totmisses );
   VG_(printf)("   cache: %,14lu Z-fetch,    %,14lu F-fetch\n",
               stats__cache_Z_fetches, stats__cache_F_fetches );
   VG_(printf)("   cache: %,14lu Z-wback,    %,14lu F-wback\n",
               stats__cache_Z_wbacks, stats__cache_F_wbacks );
   VG_(printf)("   cache: %,14lu invals,     %,14lu flushes\n",
               stats__cache_invals, stats__cache_flushes );
   VG_(printf)("   cache: %,14llu arange_New  %,14llu direct-to-Zreps\n",
               stats__cache_make_New_arange,
               stats__cache_make_New_inZrep);

   VG_(printf)("\n");
   VG_(printf)("   cline: %,10lu normalises\n",
               stats__cline_normalises );
   VG_(printf)("   cline:  rds 8/4/2/1: %,13lu %,13lu %,13lu %,13lu\n",
               stats__cline_read64s,
               stats__cline_read32s,
               stats__cline_read16s,
               stats__cline_read8s );
   VG_(printf)("   cline:  wrs 8/4/2/1: %,13lu %,13lu %,13lu %,13lu\n",
               stats__cline_write64s,
               stats__cline_write32s,
               stats__cline_write16s,
               stats__cline_write8s );
   VG_(printf)("   cline: sets 8/4/2/1: %,13lu %,13lu %,13lu %,13lu\n",
               stats__cline_set64s,
               stats__cline_set32s,
               stats__cline_set16s,
               stats__cline_set8s );
   VG_(printf)("   cline: get1s %,lu, copy1s %,lu\n",
               stats__cline_get8s, stats__cline_copy8s );
   VG_(printf)("   cline:    splits: 8to4 %,12lu    4to2 %,12lu    2to1 %,12lu\n",
              stats__cline_64to32splits,
              stats__cline_32to16splits,
              stats__cline_16to8splits );
   VG_(printf)("   cline: pulldowns: 8to4 %,12lu    4to2 %,12lu    2to1 %,12lu\n",
              stats__cline_64to32pulldown,
              stats__cline_32to16pulldown,
              stats__cline_16to8pulldown );
   if (0)
   VG_(printf)("   cline: sizeof(CacheLineZ) %ld, covers %ld bytes of arange\n",
               (Word)sizeof(CacheLineZ), (Word)N_LINE_ARANGE);

   VG_(printf)("}\n");
}

static void hg_fini ( Int exitcode )
{
   if (SHOW_DATA_STRUCTURES)
      pp_everything( PP_ALL, "SK_(fini)" );
   
   if (clo_sanity_flags)
      all__sanity_check("SK_(fini)");

   if (clo_gen_vcg > 0)
      segments__generate_vcg();


   if (1) {
      // If we expected some errors but not detected them -- complain.
      Addr ptr;
      ExpectedError *expected_error;
      HG_(initIterFM)( map_expected_errors );
      while (HG_(nextIterFM)( map_expected_errors, (Word*)&ptr,
                              (Word*)&expected_error )) {
         if(expected_error->detected == False && !expected_error->is_benign) {
            VG_(printf)("Expected race was not detected: %s:%d %p\t%s\n", 
                        expected_error->file, expected_error->line, 
                        ptr, expected_error->descr);
         }
      }
      HG_(doneIterFM) ( map_expected_errors );
   }



   if (VG_(clo_verbosity) >= 2) {
      pp_memory_usage(0, "SK_(fini)");
      pp_stats( "SK_(fini)" );  
   }
}

static void hg_pre_clo_init ( void )
{
   VG_(details_name)            ("Helgrind");
   VG_(details_version)         (NULL);
   VG_(details_description)     ("a thread error detector");
   VG_(details_copyright_author)(
      "Copyright (C) 2007-2008, and GNU GPL'd, by OpenWorks LLP et al.");
   VG_(details_bug_reports_to)  (VG_BUGS_TO);
   VG_(details_avg_translation_sizeB) ( 200 );

   VG_(basic_tool_funcs)          (hg_post_clo_init,
                                   hg_instrument,
                                   hg_fini);

   VG_(needs_core_errors)         ();
   VG_(needs_tool_errors)         (hg_eq_Error,
                                   hg_pp_Error,
                                   False,/*show TIDs for errors*/
                                   hg_update_extra,
                                   hg_recognised_suppression,
                                   hg_read_extra_suppression_info,
                                   hg_error_matches_suppression,
                                   hg_get_error_name,
                                   hg_print_extra_suppression_info);

   VG_(needs_command_line_options)(hg_process_cmd_line_option,
                                   hg_print_usage,
                                   hg_print_debug_usage);
   VG_(needs_client_requests)     (hg_handle_client_request);

   // FIXME?
   //VG_(needs_sanity_checks)       (hg_cheap_sanity_check,
   //                                hg_expensive_sanity_check);

   VG_(needs_malloc_replacement)  (hg_cli__malloc,
                                   hg_cli____builtin_new,
                                   hg_cli____builtin_vec_new,
                                   hg_cli__memalign,
                                   hg_cli__calloc,
                                   hg_cli__free,
                                   hg_cli____builtin_delete,
                                   hg_cli____builtin_vec_delete,
                                   hg_cli__realloc,
                                   HG_CLI__MALLOC_REDZONE_SZB );

   VG_(needs_var_info)();

   //VG_(needs_xml_output)          ();

   VG_(track_new_mem_startup)     ( evh__new_mem_w_perms );
   VG_(track_new_mem_stack_signal)( evh__new_mem );
   VG_(track_new_mem_brk)         ( evh__new_mem );
   VG_(track_new_mem_mmap)        ( evh__new_mem_w_perms );
   VG_(track_new_mem_stack)       ( evh__new_mem_stack );

   // FIXME: surely this isn't thread-aware
   VG_(track_copy_mem_remap)      ( shadow_mem_copy_range );

   VG_(track_change_mem_mprotect) ( evh__set_perms );

   VG_(track_die_mem_stack_signal)( evh__die_mem );
   VG_(track_die_mem_brk)         ( evh__die_mem );
   VG_(track_die_mem_munmap)      ( evh__die_mem );
   VG_(track_die_mem_stack)       ( evh__die_mem );

   // FIXME: what is this for?
   VG_(track_ban_mem_stack)       (NULL);

   VG_(track_pre_mem_read)        ( evh__pre_mem_read );
   VG_(track_pre_mem_read_asciiz) ( evh__pre_mem_read_asciiz );
   VG_(track_pre_mem_write)       ( evh__pre_mem_write );
   VG_(track_post_mem_write)      (NULL);

   /////////////////

   VG_(track_pre_thread_ll_create)( evh__pre_thread_ll_create );
   VG_(track_pre_thread_ll_exit)  ( evh__pre_thread_ll_exit );

   VG_(track_start_client_code)( evh__start_client_code );
   VG_(track_stop_client_code)( evh__stop_client_code );

   initialise_data_structures();

   /* Ensure that requirements for "dodgy C-as-C++ style inheritance"
      as described in comments at the top of pub_tool_hashtable.h, are
      met.  Blargh. */
   tl_assert( sizeof(void*) == sizeof(struct _MallocMeta*) );
   tl_assert( sizeof(UWord) == sizeof(Addr) );
   hg_mallocmeta_table
      = VG_(HT_construct)( "hg_malloc_metadata_table" );

   /* a SecMap must contain an integral number of CacheLines */
   tl_assert(0 == (N_SECMAP_ARANGE % N_LINE_ARANGE));
   /* also ... a CacheLine holds an integral number of trees */
   tl_assert(0 == (N_LINE_ARANGE % 8));
}

VG_DETERMINE_INTERFACE_VERSION(hg_pre_clo_init)

/*--------------------------------------------------------------------*/
/*--- end                                                hg_main.c ---*/
/*--------------------------------------------------------------------*/

// vim:shiftwidth=3:softtabstop=3:expandtab
