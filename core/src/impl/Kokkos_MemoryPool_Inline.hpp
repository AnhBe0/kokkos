/*
//@HEADER
// ************************************************************************
//
//                        Kokkos v. 2.0
//              Copyright (2014) Sandia Corporation
//
// Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
// the U.S. Government retains certain rights in this software.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY SANDIA CORPORATION "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SANDIA CORPORATION OR THE
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions? Contact  H. Carter Edwards (hcedwar@sandia.gov)
//
// ************************************************************************
//@HEADER
*/

#ifndef KOKKOS_MEMORYPOOL_CPP
#define KOKKOS_MEMORYPOOL_CPP

#define KOKKOS_MEMPOOLLIST_LOCK reinterpret_cast<Link*>( ~uintptr_t(0) )

// How should errors be handled?  In general, production code should return a
// value indicating failure so the user can decide how the error is handled.
// While experimental, code can abort instead.  If KOKKOS_MEMPOOLLIST_PRINTERR
// is defined, the code will abort with an error message.  Otherwise, the code
// will return with a value indicating failure when possible, or do nothing
// instead.
#define KOKKOS_MEMPOOLLIST_PRINTERR

//#define KOKKOS_MEMPOOLLIST_PRINT_INFO

//----------------------------------------------------------------------------

#if defined( KOKKOS_ACTIVE_EXECUTION_MEMORY_SPACE_CUDA )

/* This '.cpp' is being included by the header file
 * to inline these functions for Cuda.
 *
 *  Prefer to implement these functions in a separate
 *  compilation unit.  However, the 'nvcc' linker
 *  has an internal error when attempting separate compilation
 *  (--relocatable-device-code=true)
 *  of Kokkos unit tests.
 */

#define KOKKOS_MEMPOOLLIST_INLINE inline

#else

/*  This '.cpp' file is being separately compiled for the Host */

#include <Kokkos_MemoryPool.hpp>
#include <Kokkos_Atomic.hpp>

#define KOKKOS_MEMPOOLLIST_INLINE /* */

#endif

//----------------------------------------------------------------------------

namespace Kokkos {
namespace Experimental {
namespace Impl {

KOKKOS_FUNCTION
KOKKOS_MEMPOOLLIST_INLINE
void MemPoolList::insert_list( Link * lp_head, Link * lp_tail, size_t list ) const
{
  Link * volatile * freelist = m_freelist + list;

  bool inserted = false;

  while ( !inserted ) {
    Link * const old_head = *freelist;

    if ( old_head != KOKKOS_MEMPOOLLIST_LOCK ) {
      // In the initial look at the head, the freelist wasn't locked.

      // Proactively assign lp->m_next assuming a successful insertion into
      // the list.
      *reinterpret_cast< Link * volatile * >(&(lp_tail->m_next)) = old_head;

      memory_fence();

      // Attempt to insert at head of list.  If the list was changed
      // (including being locked) between the initial look and now, head will
      // be different than old_head.  This means the insert can't proceed and
      // has to be tried again.
      Link * const head = atomic_compare_exchange( freelist, old_head, lp_head );

      if ( head == old_head ) {
        inserted = true;
      }
    }
  }
}

KOKKOS_FUNCTION
KOKKOS_MEMPOOLLIST_INLINE
void * MemPoolList::allocate( size_t alloc_size ) const
{
  void * p = 0;

  bool removed = false;

  // Find the first freelist whose chunk size is big enough for allocation.
  size_t l_exp = 0;
  for ( ; m_chunk_size[l_exp] > 0 && alloc_size > m_chunk_size[l_exp]; ++l_exp );

#ifdef KOKKOS_MEMPOOLLIST_PRINTERR
  if ( m_chunk_size[l_exp] == 0 ) {
    Kokkos::abort("\n** MemoryPool::allocate() REQUESTED_SIZE_TOO_LARGE **\n" );
  }
#endif

  size_t l;

  size_t num_tries = 0;

  while ( !removed ) {
    // Keep searching for a freelist until we find one with chunks available.
    l = l_exp;
    for ( ; m_chunk_size[l] > 0 && m_freelist[l] == 0; ++l );

    if ( m_chunk_size[l] > 0 )
    {
      Link * volatile * freelist = m_freelist + l;
      Link * const old_head = *freelist;

      if ( old_head != 0 && old_head != KOKKOS_MEMPOOLLIST_LOCK ) {
        // In the initial look at the head, the freelist wasn't empty or
        // locked. Attempt to lock the head of list.  If the list was changed
        // (including being locked) between the initial look and now, head will
        // be different than old_head.  This means the removal can't proceed
        // and has to be tried again.
        Link * const head =
          atomic_compare_exchange( freelist, old_head, KOKKOS_MEMPOOLLIST_LOCK );

        if ( head == old_head ) {
          // The lock succeeded.  Get a local copy of the second entry in
          // the list.
          Link * const head_next =
            *reinterpret_cast< Link * volatile * >(&(old_head->m_next));

          // Replace the lock with the next entry on the list.
          Link * const lock_head =
            atomic_compare_exchange( freelist, KOKKOS_MEMPOOLLIST_LOCK, head_next );

          if ( lock_head != KOKKOS_MEMPOOLLIST_LOCK ) {
#ifdef KOKKOS_MEMPOOLLIST_PRINTERR
            // We shouldn't get here, but this check is here for sanity.
            printf( "\n** MemoryPool::allocate() UNLOCK_ERROR(0x%lx) **\n",
                    (unsigned long) freelist );
#ifdef KOKKOS_ACTIVE_EXECUTION_MEMORY_SPACE_HOST
            fflush( stdout );
#endif
            Kokkos::abort( "" );
#endif
          }

          *reinterpret_cast< Link * volatile * >(&(old_head->m_next)) = 0;
          p = old_head;
          removed = true;
        }
      }
      else {
        // The freelist was either locked or became empty since it was first
        // checked.  For now, the thread will just do the next loop iteration
        // to either check for the lock being released or check other
        // freelists.  If this is a performance issue, it can be revisited.
      }
    }
    else {
      if ( num_tries == 100000 ) {
        // There are no chunks large enough to satisfy the allocation in the
        // pool.  Quit and return 0.
        removed = true;

#ifdef KOKKOS_MEMPOOLLIST_PRINTERR
        Kokkos::abort("\n** MemoryPool::allocate() NO_CHUNKS_BIG_ENOUGH **\n" );
#endif
      }
      else {
        ++num_tries;
      }
    }

    if ( removed && p != 0 ) {
      // Check if too large a chunk was used because a smaller one wasn't
      // available.  If so, divide up the chunk to the next smallest chunk
      // size.  We could be more aggressive about dividing up chunks and save
      // some memory.  However, there is currently no way to combine multiple
      // free chunks into a larger single chunk which means memory
      // fragmentation could be a problem.  Being aggressive about dividing
      // up chunks would further exacerbate the fragmentation issue.
      // The way we could be more aggressive is that some chunks that need
      // the current size don't use much of it and the leftover could be
      // parceled off as a smaller chunk.  For example, let us have two chunk
      // sizes of 128 and 512 bytes.  If a request for 256 bytes comes in, it
      // requires a 512 chunk to fulfill, but the remaining 256 bytes could
      // be broken up into 2 128 byte chunks instead of wasting it as part of
      // the chunk given to fulfill the 256 byte request.
      if ( l > l_exp ) {
        // Subdivide the chunk into smaller chunks.  The first chunk will be
        // returned to satisfy the allocaiton request.  The remainder of the
        // chunks will be inserted onto the appropriate freelist.
        size_t num_chunks = m_chunk_size[l] / m_chunk_size[l_exp];
        char * pchar = (char *) p;

        // Link the chunks following the first chunk to form a list.
        for (size_t i = 2; i < num_chunks; ++i) {
          Link * chunk = (Link *) (pchar + (i - 1) * m_chunk_size[l_exp]);
          chunk->m_next = (Link *) (pchar + i * m_chunk_size[l_exp]);
        }

        Link * lp_head = (Link *) (pchar + m_chunk_size[l_exp]);
        Link * lp_tail = (Link *) (pchar + (num_chunks - 1) * m_chunk_size[l_exp]);

        // Insert the list of chunks at the head of the freelist.
        insert_list( lp_head, lp_tail, l_exp );
      }
    }
  }

#ifdef KOKKOS_MEMPOOLLIST_PRINT_INFO
#ifdef KOKKOS_ACTIVE_EXECUTION_MEMORY_SPACE_HOST
  size_t val = Kokkos::atomic_fetch_add( &m_count, 1 );
  printf( "allocate(): %4ld   l: %ld  %ld   0x%lx\n", val, l_exp, l,
          (unsigned long) p );
#else
  printf( "allocate()   l: %ld  %ld   0x%lx\n", l_exp, l, (unsigned long) p );
#endif
#endif

  return p;
}

KOKKOS_FUNCTION
KOKKOS_MEMPOOLLIST_INLINE
void MemPoolList::deallocate( void * alloc_ptr, size_t alloc_size ) const
{
#ifdef KOKKOS_MEMPOOLLIST_PRINTERR
  // Verify that the pointer is controlled by this pool.
  {
    char * ap = (char *) alloc_ptr;

    if ( ap < m_data || ap + alloc_size > m_data + m_data_size ) {
      printf( "\n** MemoryPool::deallocate() ADDRESS_OUT_OF_RANGE(0x%lx) **\n",
              (unsigned long) alloc_ptr );
#ifdef KOKKOS_ACTIVE_EXECUTION_MEMORY_SPACE_HOST
      fflush( stdout );
#endif
      Kokkos::abort( "" );
    }
  }
#endif

  // Determine which freelist to place deallocated memory on.
  size_t l = 0;
  for ( ; m_chunk_size[l] > 0 && alloc_size > m_chunk_size[l]; ++l );

#ifdef KOKKOS_MEMPOOLLIST_PRINTERR
    if ( m_chunk_size[l] == 0 ) {
      printf( "\n** MemoryPool::deallocate() CHUNK_TOO_LARGE(%ld) **\n", alloc_size );
#ifdef KOKKOS_ACTIVE_EXECUTION_MEMORY_SPACE_HOST
      fflush( stdout );
#endif
      Kokkos::abort( "" );
    }
#endif

  Link * lp = static_cast< Link * >( alloc_ptr );

  // Insert a single chunk at the head of the freelist.
  insert_list( lp, lp, l );
}


} // namespace Impl
} // namespace Experimental
} // namespace Kokkos

#undef KOKKOS_MEMPOOLLIST_LOCK
#undef KOKKOS_MEMPOOLLIST_INLINE

#ifdef KOKKOS_MEMPOOLLIST_PRINTERR
#undef KOKKOS_MEMPOOLLIST_PRINTERR
#endif

#ifdef KOKKOS_MEMPOOLLIST_PRINT_INFO
#undef KOKKOS_MEMPOOLLIST_PRINT_INFO
#endif

#endif /* #ifndef KOKKOS_MEMORYPOOL_CPP */

