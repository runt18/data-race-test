
/*--------------------------------------------------------------------*/
/*--- An AVL tree based finite map for word keys and word values.  ---*/
/*--- Inspired by Haskell's "FiniteMap" library.                   ---*/
/*---                                                  hg_wordfm.c ---*/
/*--------------------------------------------------------------------*/

/*
   This file is part of Helgrind, a Valgrind tool for detecting errors
   in threaded programs.

   Copyright (C) 2007-2008 Julian Seward
      jseward@acm.org

   This code is based on previous work by Nicholas Nethercote
   (coregrind/m_oset.c) which is

   Copyright (C) 2005-2008 Nicholas Nethercote
       njn@valgrind.org

   which in turn was derived partially from:

      AVL C library
      Copyright (C) 2000,2002  Daniel Nagy

      This program is free software; you can redistribute it and/or
      modify it under the terms of the GNU General Public License as
      published by the Free Software Foundation; either version 2 of
      the License, or (at your option) any later version.
      [...]

      (taken from libavl-0.4/debian/copyright)

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
*/

#include "pub_tool_basics.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_libcbase.h"


#ifdef HG_WORDFM_STANDALONE  // standalone compilation
// Standalone mode (for testing). 
// On x86_64 compile like this: 
//   gcc -m64 hg_wordfm.c -I../include -I../VEX/pub
//       -DVGA_amd64=1 -DHG_WORDFM_STANDALONE -g -O -Wall
# include <assert.h>
# include <string.h>
# include <stdio.h>
# include <stdlib.h>

# undef  tl_assert
# define tl_assert assert
# define vgPlain_memset memset

#endif /* def HG_WORDFM_STANDALONE */


#define HG_(str) VGAPPEND(vgHelgrind_,str)
#include "hg_wordfm.h"

//------------------------------------------------------------------//
//---                           WordFM                           ---//
//---                       Implementation                       ---//
//------------------------------------------------------------------//

/* One element of the AVL tree */
typedef
   struct _AvlNode {
      UWord key;
      UWord val;
      struct _AvlNode* child[2]; /* [0] is left subtree, [1] is right */
      Char balance; /* do not make this unsigned */
   }
   AvlNode;

typedef 
   struct {
      UWord w;
      Bool b;
   }
   MaybeWord;

#define WFM_STKMAX    32    // At most 2**32 entries can be iterated over

struct _WordFM {
   AvlNode* root;
   void*    (*alloc_nofail)( SizeT );
   void     (*dealloc)(void*);
   Word     (*kCmp)(UWord,UWord);
   AvlNode* nodeStack[WFM_STKMAX]; // Iterator node stack
   Int      numStack[WFM_STKMAX];  // Iterator num stack
   Int      stackTop;              // Iterator stack pointer, one past end
}; 

/* forward */
static Bool avl_removeroot_wrk(AvlNode** t, Word(*kCmp)(UWord,UWord));

/* Swing to the left.  Warning: no balance maintainance. */
static void avl_swl ( AvlNode** root )
{
   AvlNode* a  = *root;
   AvlNode* b  = a->child[1];
   *root       = b;
   a->child[1] = b->child[0];
   b->child[0] = a;
}

/* Swing to the right.  Warning: no balance maintainance. */
static void avl_swr ( AvlNode** root )
{
   AvlNode* a  = *root;
   AvlNode* b  = a->child[0];
   *root       = b;
   a->child[0] = b->child[1];
   b->child[1] = a;
}

/* Balance maintainance after especially nasty swings. */
static void avl_nasty ( AvlNode* root )
{
   switch (root->balance) {
      case -1: 
         root->child[0]->balance = 0;
         root->child[1]->balance = 1;
         break;
      case 1:
         root->child[0]->balance = -1;
         root->child[1]->balance = 0;
         break;
      case 0:
         root->child[0]->balance = 0;
         root->child[1]->balance = 0;
         break;
      default:
         tl_assert(0);
   }
   root->balance=0;
}

/* Find size of a non-NULL tree. */
static UWord size_avl_nonNull ( AvlNode* nd )
{
   return 1 + (nd->child[0] ? size_avl_nonNull(nd->child[0]) : 0)
            + (nd->child[1] ? size_avl_nonNull(nd->child[1]) : 0);
}

/* Unsignedly compare w1 and w2.  If w1 < w2, produce a negative
   number; if w1 > w2 produce a positive number, and if w1 == w2
   produce zero. */
static inline Word cmp_unsigned_Words ( UWord w1, UWord w2 ) {
   if (w1 < w2) return -1;
   if (w1 > w2) return 1;
   return 0;
}

/* Insert element a into the AVL tree t.  Returns True if the depth of
   the tree has grown.  If element with that key is already present,
   just copy a->val to existing node, first returning old ->val field
   of existing node in *oldV, so that the caller can finalize it
   however it wants.
*/
static 
Bool avl_insert_wrk ( AvlNode**         rootp, 
                      /*OUT*/MaybeWord* oldV,
                      AvlNode*          a, 
                      Word              (*kCmp)(UWord,UWord) )
{
   Word cmpres;

   /* initialize */
   a->child[0] = 0;
   a->child[1] = 0;
   a->balance  = 0;
   oldV->b     = False;

   /* insert into an empty tree? */
   if (!(*rootp)) {
      (*rootp) = a;
      return True;
   }

   cmpres = kCmp ? /*boxed*/   kCmp( (*rootp)->key, a->key )
                 : /*unboxed*/ cmp_unsigned_Words( (UWord)(*rootp)->key,
                                                   (UWord)a->key );

   if (cmpres > 0) {
      /* insert into the left subtree */
      if ((*rootp)->child[0]) {
         AvlNode* left_subtree = (*rootp)->child[0];
         if (avl_insert_wrk(&left_subtree, oldV, a, kCmp)) {
            switch ((*rootp)->balance--) {
               case  1: return False;
               case  0: return True;
               case -1: break;
               default: tl_assert(0);
            }
            if ((*rootp)->child[0]->balance < 0) {
               avl_swr( rootp );
               (*rootp)->balance = 0;
               (*rootp)->child[1]->balance = 0;
            } else {
               avl_swl( &((*rootp)->child[0]) );
               avl_swr( rootp );
               avl_nasty( *rootp );
            }
         } else {
            (*rootp)->child[0] = left_subtree;
         }
         return False;
      } else {
         (*rootp)->child[0] = a;
         if ((*rootp)->balance--) 
            return False;
         return True;
      }
      tl_assert(0);/*NOTREACHED*/
   }
   else 
   if (cmpres < 0) {
      /* insert into the right subtree */
      if ((*rootp)->child[1]) {
         AvlNode* right_subtree = (*rootp)->child[1];
         if (avl_insert_wrk(&right_subtree, oldV, a, kCmp)) {
            switch((*rootp)->balance++){
               case -1: return False;
               case  0: return True;
               case  1: break;
               default: tl_assert(0);
            }
            if ((*rootp)->child[1]->balance > 0) {
               avl_swl( rootp );
               (*rootp)->balance = 0;
               (*rootp)->child[0]->balance = 0;
            } else {
               avl_swr( &((*rootp)->child[1]) );
               avl_swl( rootp );
               avl_nasty( *rootp );
            }
         } else {
            (*rootp)->child[1] = right_subtree;
         }
         return False;
      } else {
         (*rootp)->child[1] = a;
         if ((*rootp)->balance++) 
            return False;
         return True;
      }
      tl_assert(0);/*NOTREACHED*/
   }
   else {
      /* cmpres == 0, a duplicate - replace the val, but don't
         incorporate the node in the tree */
      oldV->b = True;
      oldV->w = (*rootp)->val;
      (*rootp)->val = a->val;
      return False;
   }
}

/* Remove an element a from the AVL tree t.  a must be part of
   the tree.  Returns True if the depth of the tree has shrunk. 
*/
static
Bool avl_remove_wrk ( AvlNode** rootp, 
                      AvlNode*  a, 
                      Word(*kCmp)(UWord,UWord) )
{
   Bool ch;
   Word cmpres;
   cmpres = kCmp ? /*boxed*/   kCmp( (*rootp)->key, a->key )
                 : /*unboxed*/ cmp_unsigned_Words( (UWord)(*rootp)->key,
                                                   (UWord)a->key );

   if (cmpres > 0){
      /* remove from the left subtree */
      AvlNode* left_subtree = (*rootp)->child[0];
      tl_assert(left_subtree);
      ch = avl_remove_wrk(&left_subtree, a, kCmp);
      (*rootp)->child[0]=left_subtree;
      if (ch) {
         switch ((*rootp)->balance++) {
            case -1: return True;
            case  0: return False;
            case  1: break;
            default: tl_assert(0);
         }
         switch ((*rootp)->child[1]->balance) {
            case 0:
               avl_swl( rootp );
               (*rootp)->balance = -1;
               (*rootp)->child[0]->balance = 1;
               return False;
            case 1: 
               avl_swl( rootp );
               (*rootp)->balance = 0;
               (*rootp)->child[0]->balance = 0;
               return True;
            case -1:
               break;
            default:
               tl_assert(0);
         }
         avl_swr( &((*rootp)->child[1]) );
         avl_swl( rootp );
         avl_nasty( *rootp );
         return True;
      }
   }
   else
   if (cmpres < 0) {
      /* remove from the right subtree */
      AvlNode* right_subtree = (*rootp)->child[1];
      tl_assert(right_subtree);
      ch = avl_remove_wrk(&right_subtree, a, kCmp);
      (*rootp)->child[1] = right_subtree;
      if (ch) {
         switch ((*rootp)->balance--) {
            case  1: return True;
            case  0: return False;
            case -1: break;
            default: tl_assert(0);
         }
         switch ((*rootp)->child[0]->balance) {
            case 0:
               avl_swr( rootp );
               (*rootp)->balance = 1;
               (*rootp)->child[1]->balance = -1;
               return False;
            case -1:
               avl_swr( rootp );
               (*rootp)->balance = 0;
               (*rootp)->child[1]->balance = 0;
               return True;
            case 1:
               break;
            default:
               tl_assert(0);
         }
         avl_swl( &((*rootp)->child[0]) );
         avl_swr( rootp );
         avl_nasty( *rootp );
         return True;
      }
   }
   else {
      tl_assert(cmpres == 0);
      tl_assert((*rootp)==a);
      return avl_removeroot_wrk(rootp, kCmp);
   }
   return 0;
}

/* Remove the root of the AVL tree *rootp.
 * Warning: dumps core if *rootp is empty
 */
static 
Bool avl_removeroot_wrk ( AvlNode** rootp, 
                          Word(*kCmp)(UWord,UWord) )
{
   Bool     ch;
   AvlNode* a;
   if (!(*rootp)->child[0]) {
      if (!(*rootp)->child[1]) {
         (*rootp) = 0;
         return True;
      }
      (*rootp) = (*rootp)->child[1];
      return True;
   }
   if (!(*rootp)->child[1]) {
      (*rootp) = (*rootp)->child[0];
      return True;
   }
   if ((*rootp)->balance < 0) {
      /* remove from the left subtree */
      a = (*rootp)->child[0];
      while (a->child[1]) a = a->child[1];
   } else {
      /* remove from the right subtree */
      a = (*rootp)->child[1];
      while (a->child[0]) a = a->child[0];
   }
   ch = avl_remove_wrk(rootp, a, kCmp);
   a->child[0] = (*rootp)->child[0];
   a->child[1] = (*rootp)->child[1];
   a->balance  = (*rootp)->balance;
   (*rootp)    = a;
   if(a->balance == 0) return ch;
   return False;
}

static 
AvlNode* avl_find_node ( AvlNode* t, Word k, Word(*kCmp)(UWord,UWord) )
{
   if (kCmp) {
      /* Boxed comparisons */
      Word cmpresS;
      while (True) {
         if (t == NULL) return NULL;
         cmpresS = kCmp(t->key, k);
         if (cmpresS > 0) t = t->child[0]; else
         if (cmpresS < 0) t = t->child[1]; else
         return t;
      }
   } else {
      /* Unboxed comparisons */
      Word  cmpresS; /* signed */
      UWord cmpresU; /* unsigned */
      while (True) {
         if (t == NULL) return NULL; /* unlikely ==> predictable */
         cmpresS = cmp_unsigned_Words( (UWord)t->key, (UWord)k );
         if (cmpresS == 0) return t; /* unlikely ==> predictable */
         cmpresU = (UWord)cmpresS;
         cmpresU >>=/*unsigned*/ (8 * sizeof(cmpresU) - 1);
         t = t->child[cmpresU];
      }
   }
}

// Clear the iterator stack.
static void stackClear(WordFM* fm)
{
   Int i;
   tl_assert(fm);
   for (i = 0; i < WFM_STKMAX; i++) {
      fm->nodeStack[i] = NULL;
      fm->numStack[i]  = 0;
   }
   fm->stackTop = 0;
}

// Push onto the iterator stack.
static inline void stackPush(WordFM* fm, AvlNode* n, Int i)
{
   tl_assert(fm->stackTop < WFM_STKMAX);
   tl_assert(1 <= i && i <= 3);
   fm->nodeStack[fm->stackTop] = n;
   fm-> numStack[fm->stackTop] = i;
   fm->stackTop++;
}

// Pop from the iterator stack.
static inline Bool stackPop(WordFM* fm, AvlNode** n, Int* i)
{
   tl_assert(fm->stackTop <= WFM_STKMAX);

   if (fm->stackTop > 0) {
      fm->stackTop--;
      *n = fm->nodeStack[fm->stackTop];
      *i = fm-> numStack[fm->stackTop];
      tl_assert(1 <= *i && *i <= 3);
      fm->nodeStack[fm->stackTop] = NULL;
      fm-> numStack[fm->stackTop] = 0;
      return True;
   } else {
      return False;
   }
}

static 
AvlNode* avl_dopy ( AvlNode* nd, 
                    UWord(*dopyK)(UWord), 
                    UWord(*dopyV)(UWord),
                    void*(alloc_nofail)(SizeT) )
{
   AvlNode* nyu;
   if (! nd)
      return NULL;
   nyu = alloc_nofail(sizeof(AvlNode));
   tl_assert(nyu);
   
   nyu->child[0] = nd->child[0];
   nyu->child[1] = nd->child[1];
   nyu->balance = nd->balance;

   /* Copy key */
   if (dopyK) {
      nyu->key = dopyK( nd->key );
      if (nd->key != 0 && nyu->key == 0)
         return NULL; /* oom in key dcopy */
   } else {
      /* copying assumedly unboxed keys */
      nyu->key = nd->key;
   }

   /* Copy val */
   if (dopyV) {
      nyu->val = dopyV( nd->val );
      if (nd->val != 0 && nyu->val == 0)
         return NULL; /* oom in val dcopy */
   } else {
      /* copying assumedly unboxed vals */
      nyu->val = nd->val;
   }

   /* Copy subtrees */
   if (nyu->child[0]) {
      nyu->child[0] = avl_dopy( nyu->child[0], dopyK, dopyV, alloc_nofail );
      if (! nyu->child[0])
         return NULL;
   }
   if (nyu->child[1]) {
      nyu->child[1] = avl_dopy( nyu->child[1], dopyK, dopyV, alloc_nofail );
      if (! nyu->child[1])
         return NULL;
   }

   return nyu;
}

/* Initialise a WordFM. */
static void initFM ( WordFM* fm,
                     void*   (*alloc_nofail)( SizeT ),
                     void    (*dealloc)(void*),
                     Word    (*kCmp)(UWord,UWord) )
{
   fm->root         = NULL;
   fm->kCmp         = kCmp;
   fm->alloc_nofail = alloc_nofail;
   fm->dealloc      = dealloc;
   fm->stackTop     = 0;
}

/* --- Public interface functions --- */

/* Allocate and initialise a WordFM.  If kCmp is non-NULL, elements in
   the set are ordered according to the ordering specified by kCmp,
   which becomes obvious if you use VG_(initIterFM),
   VG_(initIterAtFM), VG_(nextIterFM), VG_(doneIterFM) to iterate over
   sections of the map, or the whole thing.  If kCmp is NULL then the
   ordering used is unsigned word ordering (UWord) on the key
   values. */
WordFM* HG_(newFM) ( void* (*alloc_nofail)( SizeT ),
                     void  (*dealloc)(void*),
                     Word  (*kCmp)(UWord,UWord) )
{
   WordFM* fm = alloc_nofail(sizeof(WordFM));
   tl_assert(fm);
   initFM(fm, alloc_nofail, dealloc, kCmp);
   return fm;
}

static void avl_free ( AvlNode* nd, 
                       void(*kFin)(UWord),
                       void(*vFin)(UWord),
                       void(*dealloc)(void*) )
{
   if (!nd)
      return;
   if (nd->child[0])
      avl_free(nd->child[0], kFin, vFin, dealloc);
   if (nd->child[1])
      avl_free(nd->child[1], kFin, vFin, dealloc);
   if (kFin)
      kFin( nd->key );
   if (vFin)
      vFin( nd->val );
   VG_(memset)(nd, 0, sizeof(AvlNode));
   dealloc(nd);
}

/* Free up the FM.  If kFin is non-NULL, it is applied to keys
   before the FM is deleted; ditto with vFin for vals. */
void HG_(deleteFM) ( WordFM* fm, void(*kFin)(UWord), void(*vFin)(UWord) )
{
   void(*dealloc)(void*) = fm->dealloc;
   if (fm->root)
      avl_free( fm->root, kFin, vFin, dealloc );
   VG_(memset)(fm, 0, sizeof(WordFM) );
   dealloc(fm);
}

/* Add (k,v) to fm. */
void HG_(addToFM) ( WordFM* fm, UWord k, UWord v )
{
   MaybeWord oldV;
   AvlNode* node;
   node = fm->alloc_nofail( sizeof(struct _AvlNode) );
   node->key = k;
   node->val = v;
   oldV.b = False;
   oldV.w = 0;
   avl_insert_wrk( &fm->root, &oldV, node, fm->kCmp );
   //if (oldV.b && fm->vFin)
   //   fm->vFin( oldV.w );
   if (oldV.b)
      fm->dealloc(node);
}

// Delete key from fm, returning associated key and val if found
Bool HG_(delFromFM) ( WordFM* fm,
                      /*OUT*/UWord* oldK, /*OUT*/UWord* oldV, UWord key )
{
   AvlNode* node = avl_find_node( fm->root, key, fm->kCmp );
   if (node) {
      avl_remove_wrk( &fm->root, node, fm->kCmp );
      if (oldK)
         *oldK = node->key;
      if (oldV)
         *oldV = node->val;
      fm->dealloc(node);
      return True;
   } else {
      return False;
   }
}

// Look up in fm, assigning found key & val at spec'd addresses
Bool HG_(lookupFM) ( WordFM* fm, 
                     /*OUT*/UWord* keyP, /*OUT*/UWord* valP, UWord key )
{
   AvlNode* node = avl_find_node( fm->root, key, fm->kCmp );
   if (node) {
      if (keyP)
         *keyP = node->key;
      if (valP)
         *valP = node->val;
      return True;
   } else {
      return False;
   }
}

UWord HG_(sizeFM) ( WordFM* fm )
{
   // Hmm, this is a bad way to do this
   return fm->root ? size_avl_nonNull( fm->root ) : 0;
}

Bool HG_(isEmptyFM)( WordFM* fm )
{
   return fm->root == NULL;
}

Bool HG_(anyElementOfFM) ( WordFM* fm,
                           /*OUT*/UWord* keyP, /*OUT*/UWord* valP )
{
   if (!fm->root)
      return False;
   if (keyP)
      *keyP = fm->root->key;
   if (valP)
      *valP = fm->root->val;
   return True;
}

// set up FM for iteration
void HG_(initIterFM) ( WordFM* fm )
{
   tl_assert(fm);
   stackClear(fm);
   if (fm->root)
      stackPush(fm, fm->root, 1);
}

// set up FM for iteration so that the first key subsequently produced
// by HG_(nextIterFM) is the smallest key in the map >= start_at.
// Naturally ">=" is defined by the comparison function supplied to
// HG_(newFM), as documented above.
void HG_(initIterAtFM) ( WordFM* fm, UWord start_at )
{
   Int     i;
   AvlNode *n, *t;
   Word    cmpresS; /* signed */
   UWord   cmpresU; /* unsigned */

   tl_assert(fm);
   stackClear(fm);

   if (!fm->root) 
      return;

   n = NULL;
   // We need to do regular search and fill in the stack. 
   t = fm->root;

   while (True) {
      if (t == NULL) return;

      cmpresS 
         = fm->kCmp ? /*boxed*/   fm->kCmp( t->key, start_at )
                    : /*unboxed*/ cmp_unsigned_Words( t->key, start_at );

      if (cmpresS == 0) {
         // We found the exact key -- we are done. 
         // The iteration should start with this node.
         stackPush(fm, t, 2);
         // The stack now looks like {2, 2, ... ,2, 2}
         return;
      }
      cmpresU = (UWord)cmpresS;
      cmpresU >>=/*unsigned*/ (8 * sizeof(cmpresU) - 1);
      if (!cmpresU) {
         // Push this node only if we go to the left child. 
         stackPush(fm, t, 2);
      }
      t = t->child[cmpresU];
   }
   if (stackPop(fm, &n, &i)) {
      // If we've pushed something to stack and did not find the exact key, 
      // we must fix the top element of stack. 
      tl_assert(i == 2);
      stackPush(fm, n, 3);
      // the stack looks like {2, 2, ..., 2, 3}
   }
}

// get next key/val pair.  Will tl_assert if fm has been modified
// or looked up in since initIter{,At}FM was called.
Bool HG_(nextIterFM) ( WordFM* fm, /*OUT*/UWord* pKey, /*OUT*/UWord* pVal )
{
   Int i = 0;
   AvlNode* n = NULL;
   
   tl_assert(fm);

   // This in-order traversal requires each node to be pushed and popped
   // three times.  These could be avoided by updating nodes in-situ on the
   // top of the stack, but the push/pop cost is so small that it's worth
   // keeping this loop in this simpler form.
   while (stackPop(fm, &n, &i)) {
      switch (i) {
      case 1: case_1:
         stackPush(fm, n, 2);
         /* if (n->child[0])  stackPush(fm, n->child[0], 1); */
         if (n->child[0]) { n = n->child[0]; goto case_1; }
         break;
      case 2: 
         stackPush(fm, n, 3);
         if (pKey) *pKey = n->key;
         if (pVal) *pVal = n->val;
         return True;
      case 3:
         /* if (n->child[1]) stackPush(fm, n->child[1], 1); */
         if (n->child[1]) { n = n->child[1]; goto case_1; }
         break;
      default:
         tl_assert(0);
      }
   }

   // Stack empty, iterator is exhausted, return NULL
   return False;
}

// clear the I'm iterating flag
void HG_(doneIterFM) ( WordFM* fm )
{
}

WordFM* HG_(dopyFM) ( WordFM* fm, UWord(*dopyK)(UWord), UWord(*dopyV)(UWord) )
{
   WordFM* nyu; 

   /* can't clone the fm whilst iterating on it */
   tl_assert(fm->stackTop == 0);

   nyu = fm->alloc_nofail( sizeof(WordFM) );
   tl_assert(nyu);

   *nyu = *fm;

   fm->stackTop = 0;
   VG_(memset)(fm->nodeStack, 0, sizeof(fm->nodeStack));
   VG_(memset)(fm->numStack, 0,  sizeof(fm->numStack));

   if (nyu->root) {
      nyu->root = avl_dopy( nyu->root, dopyK, dopyV, fm->alloc_nofail );
      if (! nyu->root)
         return NULL;
   }

   return nyu;
}

//------------------------------------------------------------------//
//---                         end WordFM                         ---//
//---                       Implementation                       ---//
//------------------------------------------------------------------//

//------------------------------------------------------------------//
//---                WordBag (unboxed words only)                ---//
//---                       Implementation                       ---//
//------------------------------------------------------------------//

//struct _WordBag {
//   void*   (*alloc_nofail)( SizeT );
//   void    (*dealloc)(void*);
//   UWord   firstWord;
//   UWord   firstCount;
//   WordFM* rest;
//   /* When zero, the next call to HG_(nextIterBag) gives
//      (.firstWord, .firstCount).  When nonzero, such calls traverse
//      .rest. */
//   UWord   iterCount;
//};

/* Representational invariants.  Either:

   * bag is empty
       firstWord == firstCount == 0
       rest == NULL

   * bag contains just one unique element
       firstCount > 0
       rest == NULL

   * bag contains more than one unique element
       firstCount > 0
       rest != NULL

   If rest != NULL, then 
   (1) firstWord != any .key in rest, and
   (2) all .val in rest > 0
*/

static inline Bool is_plausible_WordBag ( WordBag* bag ) {
   if (bag->firstWord == 0 && bag->firstCount == 0 && bag->rest == NULL)
      return True;
   if (bag->firstCount > 0 && bag->rest == NULL)
      return True;
   if (bag->firstCount > 0 && bag->rest != NULL)
      /* really should check (1) and (2) now, but that's
         v. expensive */
      return True;
   return False;
}

void HG_(initBag) ( WordBag* bag,
                    void* (*alloc_nofail)( SizeT ),
                    void  (*dealloc)(void*) )
{
   bag->alloc_nofail = alloc_nofail;
   bag->dealloc      = dealloc;
   bag->firstWord    = 0;
   bag->firstCount   = 0;
   bag->rest         = NULL;
   bag->iterCount    = 0;
}

void HG_(emptyOutBag) ( WordBag* bag )
{
   if (bag->rest)
      HG_(deleteFM)( bag->rest, NULL, NULL );
   /* Don't zero out the alloc and dealloc function pointers, since we
      want to be able to keep on using this bag later, without having
      to call HG_(initBag) again. */
   bag->firstWord    = 0;
   bag->firstCount   = 0;
   bag->rest         = NULL;
   bag->iterCount    = 0;
}

void HG_(addToBag)( WordBag* bag, UWord w )
{
   tl_assert(is_plausible_WordBag(bag));
   /* case where the bag is completely empty */
   if (bag->firstCount == 0) {
      tl_assert(bag->firstWord == 0 && bag->rest == NULL);
      bag->firstWord  = w;
      bag->firstCount = 1;
      return;
   }
   /* there must be at least one element in it */
   tl_assert(bag->firstCount > 0);
   if (bag->firstWord == w) {
      bag->firstCount++;
      return;
   }
   /* it's not the Distinguished Element.  Try the rest */
   { UWord key, count;
     if (bag->rest == NULL) {
        bag->rest = HG_(newFM)( bag->alloc_nofail, bag->dealloc,
                                NULL/*unboxed uword cmp*/ );
     }
     tl_assert(bag->rest);
     if (HG_(lookupFM)(bag->rest, &key, &count, w)) {
        tl_assert(key == w);
        tl_assert(count >= 1);
        HG_(addToFM)(bag->rest, w, count+1);
     } else {
        HG_(addToFM)(bag->rest, w, 1);
     }
   }
}

UWord HG_(elemBag) ( WordBag* bag, UWord w )
{
   tl_assert(is_plausible_WordBag(bag));
   if (bag->firstCount == 0) {
      return 0;
   }
   if (w == bag->firstWord) {
      return bag->firstCount;
   }
   if (!bag->rest) {
      return 0;
   }
   { UWord key, count;
     if (HG_(lookupFM)( bag->rest, &key, &count, w)) {
        tl_assert(key == w);
        tl_assert(count >= 1);
        return count;
     } else {
        return 0;
     }
   }
}

UWord HG_(sizeUniqueBag) ( WordBag* bag )
{
   tl_assert(is_plausible_WordBag(bag));
   if (bag->firstCount == 0) {
      tl_assert(bag->firstWord == 0);
      tl_assert(bag->rest == NULL);
      return 0;
   }
   return 1 + (bag->rest ? HG_(sizeFM)( bag->rest ) : 0);
}

static UWord sizeTotalBag_wrk ( AvlNode* nd )
{
   /* unchecked pre: nd is non-NULL */
   UWord w = nd->val;
   tl_assert(w >= 1);
   if (nd->child[0])
      w += sizeTotalBag_wrk(nd->child[0]);
   if (nd->child[1])
      w += sizeTotalBag_wrk(nd->child[1]);
   return w;
}
UWord HG_(sizeTotalBag)( WordBag* bag )
{
   UWord res;
   tl_assert(is_plausible_WordBag(bag));
   if (bag->firstCount == 0) {
      tl_assert(bag->firstWord == 0);
      tl_assert(bag->rest == NULL);
      return 0;
   }
   res = bag->firstCount;
   if (bag->rest && bag->rest->root)
      res += sizeTotalBag_wrk( bag->rest->root );
   return res;
}

Bool HG_(delFromBag)( WordBag* bag, UWord w )
{
   tl_assert(is_plausible_WordBag(bag));

   /* Case: bag is empty */
   if (bag->firstCount == 0) {
      /* empty */
      tl_assert(bag->firstWord == 0 && bag->rest == NULL);
      return False;
   }
   tl_assert(bag->firstCount > 0);

   /* Case: deleting from the distinguished (word,count) */
   if (w == bag->firstWord) {
      Bool  b;
      UWord tmpWord, tmpCount;
      if (bag->firstCount > 1) {
         /* Easy. */
         bag->firstCount--;
         return True;
      }
      tl_assert(bag->firstCount == 1);
      /* Now it gets complex.  Since the distinguished (word,count)
         pair is about to disappear, we have to get a new one from
         'rest'. */
      if (bag->rest == NULL) {
         /* Resulting bag really is completely empty. */
         bag->firstWord = 0;
         bag->firstCount = 0;
         return True;
      }
      /* Get a new distinguished element from 'rest'. This must be
         possible if 'rest' is non-NULL. */
      b = HG_(anyElementOfFM)( bag->rest, &bag->firstWord, &bag->firstCount );
      tl_assert(b);
      tl_assert(bag->firstCount > 0);
      b = HG_(delFromFM)( bag->rest, &tmpWord, &tmpCount, bag->firstWord );
      tl_assert(b);
      tl_assert(tmpWord == bag->firstWord);
      tl_assert(tmpCount == bag->firstCount);
      if (HG_(isEmptyFM)( bag->rest )) {
         HG_(deleteFM)( bag->rest, NULL, NULL );
         bag->rest = NULL;
      }
      return True;
   }

   /* Case: deleting from 'rest' */
   tl_assert(bag->firstCount > 0);
   tl_assert(bag->firstWord != w);
   if (bag->rest) { 
      UWord key, count;
      if (HG_(lookupFM)(bag->rest, &key, &count, w)) {
         tl_assert(key == w);
         tl_assert(count >= 1);
         if (count > 1) {
            HG_(addToFM)(bag->rest, w, count-1);
         } else {
            tl_assert(count == 1);
            HG_(delFromFM)(bag->rest, NULL, NULL, w);
            if (HG_(isEmptyFM)( bag->rest )) {
               HG_(deleteFM)( bag->rest, NULL, NULL );
               bag->rest = NULL;
            }
         }
         return True;
      } else {
         return False;
      }
   } else {
      return False;
   }
   /*NOTREACHED*/
   tl_assert(0);
}

Bool HG_(isEmptyBag)( WordBag* bag )
{
   tl_assert(is_plausible_WordBag(bag));
   if (bag->firstCount == 0) {
      tl_assert(bag->firstWord == 0);
      tl_assert(bag->rest == NULL);
      return True;
   } else {
      return False;
   }
}

Bool HG_(isSingletonTotalBag)( WordBag* bag )
{
   tl_assert(is_plausible_WordBag(bag));
   return bag->firstCount > 0 && bag->rest == NULL;
}

UWord HG_(anyElementOfBag)( WordBag* bag )
{
   tl_assert(is_plausible_WordBag(bag));
   if (bag->firstCount > 0) {
      return bag->firstWord;
   }
   /* The bag is empty, so the caller is in error, and we should
      assert. */
   tl_assert(0);
}

void HG_(initIterBag)( WordBag* bag )
{
   tl_assert(is_plausible_WordBag(bag));
   bag->iterCount = 0;
}

Bool HG_(nextIterBag)( WordBag* bag, /*OUT*/UWord* pVal, /*OUT*/UWord* pCount )
{
   Bool b;
   if (bag->iterCount == 0) {
      /* Emitting (.firstWord, .firstCount) if we have it. */
      if (bag->firstCount == 0) {
         /* empty */
         return False;
      }
      if (pVal) *pVal = bag->firstWord;
      if (pCount) *pCount = bag->firstCount;
      bag->iterCount = 1;
      return True;
   }

   /* else emitting from .rest, if present */
   if (!bag->rest)
      return False;

   if (bag->iterCount == 1)
      HG_(initIterFM)( bag->rest );

   b = HG_(nextIterFM)( bag->rest, pVal, pCount );
   bag->iterCount++;

   return b;
}

void HG_(doneIterBag)( WordBag* bag )
{
   bag->iterCount = 0;
   if (bag->rest)
      HG_(doneIterFM)( bag->rest );
}


//------------------------------------------------------------------//
//---             end WordBag (unboxed words only)               ---//
//---                       Implementation                       ---//
//------------------------------------------------------------------//


#ifdef HG_WORDFM_STANDALONE

//------------------------------------------------------------------//
//---                      Simple test driver.                   ---//
//------------------------------------------------------------------//

// We create a map with N values {1, 3, 5, ..., (N*2-1)}
// and do some trivial stuff with it. 


// Return the number of elements in range [beg, end). 
// Just lookup for each element in range and count. 
int search_all_elements_in_range_1(WordFM *map, long beg, long end)
{
   long n_found = 0;
   long i;
   for (i = beg; i < end; i++) {
      UWord key, val;
      if (HG_(lookupFM)(map, &key, &val, (Word)i)) {
         n_found++;
         assert(key == -val);
         assert(key == (UWord)i);
      }
   }
   return n_found;
}

// Return the number of elements in range [beg, end). 
// Start with the largest element 'e' such that 'e <= beg' 
// and iterate until 'e < end'. 
int search_all_elements_in_range_2(WordFM *map, long beg, long end)
{
   int n_found = 0;
   UWord key, val;
   HG_(initIterAtFM)(map, beg);
   while (HG_(nextIterFM)(map, &key, &val) && (long)key < end) {
      assert(key == -val);
      n_found++;
   }
   HG_(doneIterFM)(map);
   return n_found;
}

void showBag ( WordBag* bag )
{
   UWord val, count;
   printf("Bag{");
   HG_(initIterBag)( bag );
   while (HG_(nextIterBag)( bag, &val, &count )) {
      printf(" %lux%lu ", count, val );
   }
   HG_(doneIterBag)( bag );
   printf("}"); fflush(stdout);
}

int main(void)
{
   long i, n = 10;
   UWord key, val;
   long beg, end;

   printf("Create the map, n=%ld\n", n);
   WordFM *map = HG_(newFM)(malloc, free, NULL/*unboxed Word cmp*/);

   printf("Add keys: ");
   for(i = 0; i < n; i++) {
      long val = i * 2 + 1; // 1, 3, 5, ... (n*2-1)
      printf("%ld ", val);
      HG_(addToFM)(map, val, -val);
   }
   assert(HG_(sizeFM)(map) == (UWord)n);
   printf("\n");
   printf("Iterate elements, size=%d\n", (int)HG_(sizeFM)(map));
   HG_(initIterFM)(map);

   while (HG_(nextIterFM(map, &key, &val))) {
   //   int j;
   //   printf("Stack k=%d\n", (int)key);
   //   for(j = map->stackTop-1; j >= 0; j--) {
   //      printf("\t[%d]: k=%d s=%d\n", j,
   //             (int)map->nodeStack[j]->key, (int)map->numStack[j]);
   //   }
      assert(key == -val);
   }
   HG_(doneIterFM)(map);

   printf("Test initIterAtFM\n");
   for(beg = 0; beg <= n*2; beg++) {
      HG_(initIterAtFM)(map, (Word)beg);
      int prev = -1; 
      printf("StartWith: %ld: ", beg);
      int n_iter = 0;

      while(HG_(nextIterFM(map, &key, &val))) {
         printf("%d ", (int)key);
         assert(key == -val);
         if(prev > 0) assert(prev + 2 == (int)key);
         prev = (int)key;
         n_iter++;
      }
      HG_(doneIterFM)(map);

      printf("\ntotal: %d\n", n_iter);
      if      (beg < 1   ) assert(n_iter == n);
      else if (beg >= n*2) assert(n_iter == 0);
      else                 assert(n_iter == (n - beg/2));
   }

   printf("Compare search_all_elements_in_range_[12]\n");
   for (beg = 0; beg <= n*2; beg++) {
      for (end = 0; end <= n*2; end++) {
         assert(   search_all_elements_in_range_1(map, beg, end) 
                == search_all_elements_in_range_2(map, beg, end));
      }
   }

   printf("Delete the map\n");
   HG_(deleteFM)(map, NULL, NULL);
   printf("Ok!\n");

   printf("\nBEGIN testing WordBag\n");
   WordBag bag;
   Bool b;

   HG_(initBag)( &bag, malloc, free );
   
   printf("operations on an empty bag\n");
   printf(" show:       " ); showBag( &bag ); printf("\n");
   printf(" elem:       %lu\n", HG_(elemBag)( &bag, 42 ));
   printf(" isEmpty:    %lu\n", (UWord) HG_(isEmptyBag)( &bag ));
   printf(" iSTB:       %lu\n", (UWord) HG_(isSingletonTotalBag)( &bag ));
   printf(" sizeUnique: %lu\n", HG_(sizeUniqueBag)( &bag ));
   printf(" sizeTotal:  %lu\n", HG_(sizeTotalBag)( &bag ));
   printf(" delFrom:    %lu\n", (UWord)HG_(delFromBag)( &bag, 42 ));

   assert( HG_(isEmptyBag)( &bag ));
   printf("\noperations on bag { 41 }\n");
   HG_(addToBag)( &bag, 41 );
   printf(" show:       " ); showBag( &bag ); printf("\n");
   printf(" elem:       %lu\n", HG_(elemBag)( &bag, 42 ));
   printf(" isEmpty:    %lu\n", (UWord) HG_(isEmptyBag)( &bag ));
   printf(" iSTB:       %lu\n", (UWord) HG_(isSingletonTotalBag)( &bag ));
   printf(" sizeUnique: %lu\n", HG_(sizeUniqueBag)( &bag ));
   printf(" sizeTotal:  %lu\n", HG_(sizeTotalBag)( &bag ));
   printf(" delFrom:    %lu\n", (UWord)HG_(delFromBag)( &bag, 42 ));

   b = HG_(delFromBag)( &bag, 41 ); assert(b);

   printf("\noperations on bag { 41,41 }\n");
   HG_(addToBag)( &bag, 41 );
   HG_(addToBag)( &bag, 41 );
   printf(" show:       " ); showBag( &bag ); printf("\n");
   printf(" elem:       %lu\n", HG_(elemBag)( &bag, 42 ));
   printf(" isEmpty:    %lu\n", (UWord) HG_(isEmptyBag)( &bag ));
   printf(" iSTB:       %lu\n", (UWord) HG_(isSingletonTotalBag)( &bag ));
   printf(" sizeUnique: %lu\n", HG_(sizeUniqueBag)( &bag ));
   printf(" sizeTotal:  %lu\n", HG_(sizeTotalBag)( &bag ));
   printf(" delFrom:    %lu\n", (UWord)HG_(delFromBag)( &bag, 42 ));

   printf("\noperations on bag { 41,41, 42, 43,43 }\n");
   HG_(addToBag)( &bag, 42 );
   HG_(addToBag)( &bag, 43 );
   HG_(addToBag)( &bag, 43 );
   printf(" show:       " ); showBag( &bag ); printf("\n");
   printf(" elem:       %lu\n", HG_(elemBag)( &bag, 42 ));
   printf(" isEmpty:    %lu\n", (UWord) HG_(isEmptyBag)( &bag ));
   printf(" iSTB:       %lu\n", (UWord) HG_(isSingletonTotalBag)( &bag ));
   printf(" sizeUnique: %lu\n", HG_(sizeUniqueBag)( &bag ));
   printf(" sizeTotal:  %lu\n", HG_(sizeTotalBag)( &bag ));
   printf(" delFrom:    %lu\n", (UWord)HG_(delFromBag)( &bag, 42 ));

   b = HG_(delFromBag)( &bag, 41 ); assert(b);
   printf(" after del of 41: " ); showBag( &bag ); printf("\n");
   b = HG_(delFromBag)( &bag, 41 ); assert(b);
   printf(" after del of 41: " ); showBag( &bag ); printf("\n");
   b = HG_(delFromBag)( &bag, 43 ); assert(b);
   printf(" after del of 43: " ); showBag( &bag ); printf("\n");
   b = HG_(delFromBag)( &bag, 42 ); assert(!b); // already gone
   printf(" after del of 42: " ); showBag( &bag ); printf("\n");
   b = HG_(delFromBag)( &bag, 43 ); assert(b);
   printf(" after del of 43: " ); showBag( &bag ); printf("\n");

   HG_(emptyOutBag)( &bag );

   printf("\noperations on now empty bag\n");
   printf(" show:       " ); showBag( &bag ); printf("\n");
   printf(" elem:       %lu\n", HG_(elemBag)( &bag, 42 ));
   printf(" isEmpty:    %lu\n", (UWord) HG_(isEmptyBag)( &bag ));
   printf(" iSTB:       %lu\n", (UWord) HG_(isSingletonTotalBag)( &bag ));
   printf(" sizeUnique: %lu\n", HG_(sizeUniqueBag)( &bag ));
   printf(" sizeTotal:  %lu\n", HG_(sizeTotalBag)( &bag ));
   printf(" delFrom:    %lu\n", (UWord)HG_(delFromBag)( &bag, 42 ));

   printf("\nEND testing WordBag\n");

   return 0;
}

#endif

/*--------------------------------------------------------------------*/
/*--- end                                              hg_wordfm.c ---*/
/*--------------------------------------------------------------------*/
