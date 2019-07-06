/*---------------------------------------------------------------------------*
 *
 *  tempheap.cpp: a heap for temporary data
 *  date:	3 January 1995
 *  authors:	Giuseppe Attardi and Tito Flagella
 *  email:	cmm@di.unipi.it
 *  address:	Dipartimento di Informatica
 *		Corso Italia 40
 *		I-56125 Pisa, Italy
 *
 *  Copyright (C) 1993, 1994, 1995 Giuseppe Attardi and Tito Flagella.
 *
 *  This file is part of the PoSSo Customizable Memory Manager (CMM).
 *
 * Permission to use, copy, and modify this software and its documentation is
 * hereby granted only under the following terms and conditions.  Both the
 * above copyright notice and this permission notice must appear in all copies
 * of the software, derivative works or modified versions, and any portions
 * thereof, and both notices must appear in supporting documentation.
 *
 * Users of this software agree to the terms and conditions set forth herein,
 * and agree to license at no charge to all parties under these terms and
 * conditions any derivative works or modified versions of this software.
 * 
 * This software may be distributed (but not offered for sale or transferred
 * for compensation) to third parties, provided such third parties agree to
 * abide by the terms and conditions of this notice.  
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE COPYRIGHT HOLDERS DISCLAIM ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS.   IN NO EVENT SHALL THE COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR
 * ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *---------------------------------------------------------------------------*/

#include "tempheap.h"

/*---------------------------------------------------------------------------*
 *
 * -- Container
 *
 *---------------------------------------------------------------------------*/

Container::Container(int bytes, CmmHeap *heap)
{
  int pages = (bytes + bytesPerPage - 1) / bytesPerPage;
  body = allocatePages(pages, heap);
  size = pages * wordsPerPage;
  top = BOTTOM;
}

/*---------------------------------------------------------------------------*
 *
 * -- Container::reset()
 *
 *  Both body and size must be multiple of bitsPerWord.
 *
 *---------------------------------------------------------------------------*/

void
Container::reset()
{
  bzero((char*)&objectMap[WORD_INDEX(body)],
	((usedWords() + bitsPerWord - 1) / bitsPerWord) * bytesPerWord);
#if !HEADER_SIZE || defined(MARKING)
  resetliveMap();
#endif
  top = BOTTOM;
}

#if !HEADER_SIZE || defined(MARKING)
void
Container::resetliveMap()
{
  bzero((char*)&liveMap[WORD_INDEX(body)],
	((usedWords() + bitsPerWord - 1) / bitsPerWord) * bytesPerWord);
}
#endif

/*---------------------------------------------------------------------------*
 * -- Container::alloc
 *
 * This function does not check for the available space.
 * The caller must use room() to verify that.
 *
 * Side effects: top
 *
 *---------------------------------------------------------------------------*/

GCP Container::alloc(int bytes)
{
  GCP object = body + top;
  int words = bytesToWords(bytes);
  top += words;
  ALLOC_SETUP(object, words);
#if HEADER_SIZE && defined(DOUBLE_ALIGN)
  if ((top & 1) == 0 && top < size) top++;
#endif
  return(object);
}

/*---------------------------------------------------------------------------*
 * -- Container::copy
 *
 * copies object into the container
 * This function does not check for the available space.
 * The caller must use room() to verify that.
 *
 * Side effects: top
 *
 *---------------------------------------------------------------------------*/

CmmObject *
Container::copy(CmmObject *ptr)
{
  GCP object = body + top;
  register int words = ptr->words();
  top += words;
  register GCP scan = object;
#if HEADER_SIZE
  // Copy the header as well which is included in words
  ptr -= HEADER_SIZE;
  object += HEADER_SIZE;
#endif
  while (words--) *scan++ = *(GCP)ptr++;
  SET_OBJECTMAP(object);
#if HEADER_SIZE && defined(DOUBLE_ALIGN)
  // Since collect() will scan toSpace, we must fill in the gaps:
  if ((top & 1) == 0 && top < size)
    *((GCP)(body + top++)) = MAKE_HEADER(1, MAKE_TAG(1));
#endif
  return (CmmObject *)object;
}


/*---------------------------------------------------------------------------*
 *
 * -- RootSet
 *
 *---------------------------------------------------------------------------*/

RootSet::RootSet()
{
  // Default to non conservative
  isConservative = false;
}

void
RootSet::scan(CmmHeap *heap)
{
  CmmObject *objPtr, **objPtrLoc;
  CmmHeap *oldHeap = Cmm::heap;
  Cmm::heap = heap;
  roots.begin();
  while (objPtr = roots.get()) objPtr->traverse();
  rootsLoc.begin();
  while (objPtrLoc = rootsLoc.get())
    heap->scavenge(objPtrLoc);
  Cmm::heap = oldHeap;
}


/*---------------------------------------------------------------------------*
 *
 * -- TempHeap
 *
 *---------------------------------------------------------------------------*/

void
TempHeap::expand()
{
  if (current == chunkNum - 1)
    {
      toCollect = true;
      
      Container **nc = new Container*[chunkNum + chunkInc];
      int i;
      for (i = 0;  i <= current;  i++)
	nc[i] = chunk[i];
      delete chunk;
      chunkNum += chunkInc;
      for (; i < chunkNum; i++)
	nc[i] = new Container(chunkSize, this);
      chunk = nc;
    }
  current++;
}

GCP
TempHeap::alloc(unsigned long bytes)
{ 
  if ((chunk[current]->room()) < bytesToWords(bytes) * bytesPerWord)
    expand();			// expand() modifies current.

  return chunk[current]->alloc(bytes);
}

CmmObject *
TempHeap::copy(CmmObject *ptr)
{
  if (chunk[current]->room() < ptr->size())
    expand();
  return chunk[current]->copy(ptr);
}

void
TempHeap::scavenge(CmmObject **loc)
{
  GCP pp = (GCP)*loc;
  if (OUTSIDE_HEAPS(GCPtoPage(pp)))
    return;

  CmmObject *oldPtr = (CmmObject *)basePointer(pp);
  int offset = (Word)pp - (Word)oldPtr;

  if (!inside(oldPtr))
    {
      /* If an external object may point into this heap, we should visit.
	 However this may loop if there are cyclic objects and visit()
	 does not perform marking.
	 Not performing visit() means to assume that no pointers into this
	 heap, except the roots.
	 */
#     ifdef MARKING
      visit(oldPtr);
#     endif
      return;
    }
  if
#   if HEADER_SIZE
    (oldPtr->forwarded())
#   else
      (MARKED(oldPtr))
#   endif
	*loc = (CmmObject *)((Word)oldPtr->getForward() + offset);
  else
    {
      CmmObject *newObj = copy(oldPtr);
      oldPtr->setForward(newObj);
      *loc = (CmmObject *)((Word)newObj + offset);
    }
}

void
TempHeap::collect()
{
  CmmObject *objPtr;
  int i;

  if (!toCollect && current < chunkNum * 0.8)
    return;

  WHEN_VERBOSE(CMM_STATS,
	       fprintf(stderr, "TempHeap Collector: C[0-%d] -> ", current));

#if !HEADER_SIZE || defined(MARKING)
  /* Clear the liveMap bitmap */
  for (i = 0; i <= current; i++)
    chunk[i]->resetliveMap();
#endif

  // Expand is used to start with a new Container which will
  // be the first one of the ToSpace.
  // A simple increment of current is not enough when the stack needs
  // to be expanded.

  expand();
  int toSpaceIndex = current;

  roots.scan(this);

  for (i = toSpaceIndex; i <= current; i++)
    {
      Container *container = chunk[i];
      objPtr = container->bottom();
#if !HEADER_SIZE
      SET_OBJECTMAP(container->current()); // ensure that we stop at the end
#endif
      while (objPtr < container->current())
	{
#if HEADER_SIZE && defined(DOUBLE_ALIGN)
	  if (HEADER_TAG(*(GCP)(objPtr - HEADER_SIZE)) > 1)
#endif
	    objPtr->traverse();
	  objPtr = objPtr->next();
	}
    }
  WHEN_VERBOSE(CMM_STATS,
	       fprintf(stderr, "C[%d-%d]\n", toSpaceIndex, current));

  for (i = 0 ; i < toSpaceIndex; i++)
    chunk[i]->reset();

  for (i = 0; i <= current - toSpaceIndex; i++)
    { 
      Container *tmp = chunk[i];
      chunk[i] = chunk[toSpaceIndex + i];
      chunk[toSpaceIndex + i] = tmp;
    }
  current -= toSpaceIndex;
  toCollect = false;		// It's better at the end, because expand 
				// could reset it to true.
}

void
TempHeap::reset()
{
  // It resets the objectMap. In fact collect is
  // expected to coexist with reset. Use weakReset elsewhere.

  WHEN_VERBOSE(CMM_STATS,
	       fprintf(stderr, "Resetting TempHeap: C[0-%d]\n", current));

  for (int i = 0; i <= current; i++)
    chunk[i]->reset();

  current = 0;
}

void
TempHeap::weakReset()
{
  // It doesn't reset the objectMap. In fact collect is not
  // expected to coexist with weakReset. And objectMap is only used
  // by collect.

  WHEN_VERBOSE(CMM_STATS,
	       fprintf(stderr, "Weak Resetting TempHeap: C[0-%d]\n", current));

  for (int i = 0; i <= current; i++)
    chunk[i]->weakReset();

  current = 0;
}
