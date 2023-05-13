/*
** 2023 May 8
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file implements a simple resource management package, interface
** and function of which is described in resmanage.h .
*/

#include <stdlib.h>
#include <stdio.h>

#include <assert.h>
#include "resmanage.h"

/* Track how to free various held resources. */
typedef enum FreeableResourceKind {
  FRK_Indirect = 1<<15,
  FRK_Malloc = 0, FRK_DbConn = 1, FRK_DbStmt = 2,
  FRK_DbMem = 3, FRK_File = 4,
#if (!defined(_WIN32) && !defined(WIN32)) || !SQLITE_OS_WINRT
  FRK_Pipe,
#endif
#ifdef SHELL_MANAGE_TEXT
  FRK_Text,
#endif
  FRK_AnyRef,
  FRK_CustomBase /* series of values for custom freers */
} FreeableResourceKind;

#if defined(_WIN32) || defined(WIN32)
# if !SQLITE_OS_WINRT
#  include <io.h>
#  include <fcntl.h>
#  undef pclose
#  define pclose _pclose
# endif
#else
extern int pclose(FILE*);
#endif

typedef struct ResourceHeld {
  union {
    void *p_any;
    void *p_malloced;
    sqlite3 *p_conn;
    sqlite3_stmt *p_stmt;
    void *p_s3mem;
    FILE *p_stream;
#ifdef SHELL_MANAGE_TEXT
    ShellText *p_text;
#endif
    AnyResourceHolder *p_any_rh;
  } held;
  FreeableResourceKind frk;
} ResourceHeld;

/* The held-resource stack. This is for single-threaded use only. */
static ResourceHeld *pResHold = 0;
static ResourceCount numResHold = 0;
static ResourceCount numResAlloc = 0;

/* A small set of custom freers. It is linearly searched, used for
** layered heap-allocated (and other-allocated) data structures, so
** tends to have use limited to where slow things are happening.
*/
static ResourceCount numCustom = 0; /* number of the set */
static ResourceCount numCustomAlloc = 0; /* allocated space */
typedef void (*FreerFunction)(void *);
static FreerFunction *aCustomFreers = 0; /* content of set */

const char *resmanage_oom_message = "out of memory, aborting";

/* Info recorded in support of quit_moan(...) and stack-ripping */
static RipStackDest *pRipStack = 0;

/* Current position of the held-resource stack */
ResourceMark holder_mark(){
  return numResHold;
}

/* Strip resource stack then strip call stack (or exit.) */
void quit_moan(const char *zMoan, int errCode){
  RipStackDest *pRSD = pRipStack;
  int nFreed;
  if( zMoan ){
    fprintf(stderr, "Error: Terminating due to %s.\n", zMoan);
  }
  fprintf(stderr, "Auto-freed %d resources.\n",
          holder_free( (pRSD)? pRSD->resDest : 0 ));
  pRipStack = (pRSD)? pRSD->pPrev : 0;
#ifndef SHELL_OMIT_LONGJMP
  if( pRSD!=0 ){
    longjmp(pRSD->exeDest, errCode);
  } else
#endif
    exit(errCode);
}

/* Free a single resource item. (ignorant of stack) */
static int free_rk( ResourceHeld *pRH ){
  int rv = 0;
  if( pRH->held.p_any == 0 ) return rv;
  if( pRH->frk & FRK_Indirect ){
    pRH->held.p_any = *(void**)pRH->held.p_any;
    if( pRH->held.p_any == 0 ) return rv;
    pRH->frk &= ~FRK_Indirect;
  }
  ++rv;
  switch( pRH->frk ){
  case FRK_Malloc:
    free(pRH->held.p_malloced);
    break;
  case FRK_DbConn:
    sqlite3_close_v2(pRH->held.p_conn);
    break;
  case FRK_DbStmt:
    sqlite3_clear_bindings(pRH->held.p_stmt);
    sqlite3_finalize(pRH->held.p_stmt);
    break;
  case FRK_DbMem:
    sqlite3_free(pRH->held.p_s3mem);
    break;
  case FRK_File:
    fclose(pRH->held.p_stream);
    break;
#if (!defined(_WIN32) && !defined(WIN32)) || !SQLITE_OS_WINRT
  case FRK_Pipe:
    pclose(pRH->held.p_stream);
    break;
#endif
#ifdef SHELL_MANAGE_TEXT
  case FRK_Text:
    freeText(pRH->held.p_text);
    break;
#endif
  case FRK_AnyRef:
    (pRH->held.p_any_rh->its_freer)(pRH->held.p_any_rh->pAny);
    break;
  default:
    {
      int ck = pRH->frk - FRK_CustomBase;
      assert(ck>=0);
      if( ck < numCustom ){
        aCustomFreers[ck]( pRH->held.p_any );
      }else --rv;
    }
  }
  pRH->held.p_any = 0;
  return rv;
}

/* Take back a held resource pointer, leaving held as NULL. (no-op) */
void* take_held(ResourceMark mark, ResourceCount offset){
  return swap_held(mark,offset,0);
}

/* Swap a held resource pointer for a new one. */
void* swap_held(ResourceMark mark, ResourceCount offset, void *pNew){
  ResourceCount rix = mark + offset;
  assert(rix < numResHold && numResHold > 0);
  if( rix < numResHold && numResHold > 0 ){
    void *rv = pResHold[rix].held.p_any;
    pResHold[rix].held.p_any = pNew;
    return rv;
  }else return 0;
}

/* Reserve room for at least count additional holders, with no chance
** of an OOM abrupt exit. This is used internally only by this package.
** The return is either 1 for success, or 0 indicating an OOM failure.
 */
static int more_holders_try(ResourceCount count){
  ResourceHeld *prh;
  ResourceCount newAlloc = numResHold + count;
  if( newAlloc < numResHold ) return 0; /* Overflow, sorry. */
  if( newAlloc <= numResAlloc ) return 1;
  prh = (ResourceHeld*)realloc(pResHold, newAlloc*sizeof(ResourceHeld));
  if( prh == 0 ) return 0;
  pResHold = prh;
  numResAlloc = newAlloc;
  return 1;
}

/* Lose one or more holders. */
void* pop_holder(void){
  assert(numResHold>0);
  if( numResHold>0 ) return pResHold[--numResHold].held.p_any;
  return 0;
}
void pop_holders(ResourceCount num){
  assert(numResHold>=num);
  if( numResHold>=num ) numResHold -= num;
  else numResHold = 0;
}

/* Shared resource-stack pushing code */
static void res_hold(void *pv, FreeableResourceKind frk){
  ResourceHeld rh = { pv, frk };
  if( numResHold == numResAlloc ){
    ResourceCount nrn = (ResourceCount)((numResAlloc>>2) + 5);
    if( !more_holders_try(nrn) ){
      free_rk(&rh);
      quit_moan(resmanage_oom_message,1);
    }
  }
  pResHold[numResHold++] = rh;
}

/* Hold a NULL (Assure swap_held() cannot fail.) */
void more_holders(ResourceCount more){
  if( !more_holders_try(more) ) quit_moan(resmanage_oom_message,1);
}

/* Hold anything in the malloc() heap */
void* mmem_holder(void *pm){
  res_hold(pm, FRK_Malloc);
  return pm;
}
/* Hold a C string in the malloc() heap */
char* mstr_holder(char *z){
  res_hold(z, FRK_Malloc);
  return z;
}
/* Hold a C string in the SQLite heap */
char* sstr_holder(char *z){
  res_hold(z, FRK_DbMem);
  return z;
}
/* Hold a C string in the SQLite heap, reference to */
void sstr_ptr_holder(char **pz){
  assert(pz!=0);
  res_hold(pz, FRK_DbMem|FRK_Indirect);
}
/* Hold an open C runtime FILE */
void file_holder(FILE *pf){
  res_hold(pf, FRK_File);
}
#if (!defined(_WIN32) && !defined(WIN32)) || !SQLITE_OS_WINRT
/* Hold an open C runtime pipe */
void pipe_holder(FILE *pp){
  res_hold(pp, FRK_Pipe);
}
#endif
#ifdef SHELL_MANAGE_TEXT
/* a ShellText object, reference to (storage for which not managed) */
static void text_ref_holder(ShellText *pt){
  res_hold(pt, FRK_Text);
}
#endif
/* Hold anything together with arbitrary freeing function */
void* any_holder(void *pm, void (*its_freer)(void*)){
  int i = 0;
  while( i < numCustom ){
    if( its_freer == aCustomFreers[i] ) break;
    ++i;
  }
  if( i == numCustom ){
    size_t ncf = numCustom + 2;
    FreerFunction *pcf;
    pcf = (FreerFunction *)realloc(aCustomFreers, ncf*sizeof(FreerFunction));
    if( pcf!=0 ){
      assert(ncf < (1<<16));
      numCustomAlloc = (ResourceCount)ncf;
      aCustomFreers = pcf;
      aCustomFreers[numCustom++] = its_freer;
    }else{
      quit_moan(resmanage_oom_message,1);
    }
  }
  res_hold(pm, i + FRK_CustomBase);
  return pm;
}
/* Hold some SQLite-allocated memory */
void* smem_holder(void *pm){
  res_hold(pm, FRK_DbMem);
  return pm;
}
/* Hold a SQLite database "connection" */
void conn_holder(sqlite3 *pdb){
  res_hold(pdb, FRK_DbConn);
}
/* Hold a SQLite prepared statement */
void stmt_holder(sqlite3_stmt *pstmt){
  res_hold(pstmt, FRK_DbStmt);
}
/* Hold a SQLite prepared statement, reference to */
void stmt_ptr_holder(sqlite3_stmt **ppstmt){
  assert(ppstmt!=0);
  res_hold(ppstmt, FRK_DbStmt|FRK_Indirect);
}
/* Hold a reference to an AnyResourceHolder (in stack frame) */
void any_ref_holder(AnyResourceHolder *parh){
  assert(parh!=0 && parh->its_freer!=0);
  res_hold(parh, FRK_AnyRef);
}

/* Free all held resources in excess of given resource stack mark,
** then return how many needed freeing. */
int holder_free(ResourceMark mark){
  int rv = 0;
  while( numResHold > mark ){
    rv += free_rk(&pResHold[--numResHold]);
  }
  if( mark==0 ){
    if( numResAlloc>0 ){
      free(pResHold);
      pResHold = 0;
      numResAlloc = 0;
    }
    if( numCustomAlloc>0 ){
      free(aCustomFreers);
      aCustomFreers = 0;
      numCustom = 0;
      numCustomAlloc = 0;
    }
  }
  return rv;
}

/* Record a resource stack and call stack rip-to position */
void register_exit_ripper(RipStackDest *pRSD){
  assert(pRSD!=0);
  pRSD->pPrev = pRipStack;
  pRSD->resDest = holder_mark();
  pRipStack = pRSD;
}
/* Undo register_exit_ripper effect, back to previous state. */
void forget_exit_ripper(RipStackDest *pRSD){
  if( pRSD==0 ) pRipStack = 0;
  else{
    pRipStack = pRSD->pPrev;
    pRSD->pPrev = 0;
  }
}
