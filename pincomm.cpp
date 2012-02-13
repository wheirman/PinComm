/* $Id: pincomm.cpp 6478 2010-05-25 12:11:11Z wheirman $ */

#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <deque>
#include <assert.h>
#include "pin.H"
#include "pinmagic.h"
#include "binstore.h"



/* Output record types

A   call site
C   communication record
E   function entry
F   function name
G   region change
I   instruction count
J   jump (stack mismatch)
M   malloc
N   free
R   read
S   stack contents
T   set region
W   write
X   function exit

START start
STOP  stop
END   end

*/


BINSTORE * trace;



#define MAX_THREADS 1024
#define MAX_STACK 1024
#define MAX_MREGION (1<<22)


KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool",
    "o", "pincommtrace.pcs", "specify output file name");
KNOB<string> KnobOutputCmd(KNOB_MODE_WRITEONCE, "pintool",
    "c", "", "specify command to pipe output to");
KNOB<BOOL> KnobUseMagic(KNOB_MODE_WRITEONCE, "pintool",
    "magic", "0", "use Simics Magic instruction to start/stop measurement");
KNOB<INT> KnobZone(KNOB_MODE_WRITEONCE, "pintool",
    "zone", "0", "only measure zone <zone>");
KNOB<BOOL> KnobIgnoreComm(KNOB_MODE_WRITEONCE, "pintool",
    "nocomm", "0", "don't measure communication (only call tree and malloc()s)");
KNOB<UINT> KnobMinLen(KNOB_MODE_WRITEONCE, "pintool",
    "minlen", "0", "combine regions until minimum length (instruction count) is <minlen>");
KNOB<UINT> KnobRegionTime(KNOB_MODE_WRITEONCE, "pintool",
    "regiontime", "0", "split regions into chunks of <regiontime> instructions (replaces MAGICly marked regions)");
KNOB<UINT> KnobMemGran(KNOB_MODE_WRITEONCE, "pintool",
    "memgran", "64", "memory granularity (default: 64)");
KNOB<BOOL> KnobRegionOnly(KNOB_MODE_WRITEONCE, "pintool",
    "regiononly", "0", "only measure inter-region communication, output in csv format to stdout");
KNOB<string> KnobCsvOutputFile(KNOB_MODE_WRITEONCE, "pintool",
    "csv", "pincommtrace.csv", "output file name for CSV output");


/* lock to put around writing output, so lines from separate threads don't intermingle */
inline void L() { PIN_LockClient(); }
inline void U() { PIN_UnlockClient(); }


static enum { S_INIT, S_MEASURE, S_DONE } state;

static UINT64 icount[MAX_THREADS] = { 0 };
static UINT64 icount_tot = 0, icount_next = 0;
static UINT64 icount_read = 0, bcount_read = 0;
static UINT64 icount_read_cache = 0, bcount_read_cache = 0;

static int memgran_bits;

struct stackItemType {
  UINT32 funcid;
  ADDRINT sp;
  UINT32 returnIp;
  UINT32 dfuncid;
  UINT32 mregion;
  UINT64 icount_start;
  UINT64 icounttot_start;
  BOOL output;
};
typedef std::deque<stackItemType> threadStackType;
typedef std::map<THREADID, threadStackType> callStackType;
static callStackType callStack;


static std::map<THREADID, UINT32> dfuncid;
static std::map<THREADID, UINT64> region;
static std::map<ADDRINT, UINT64> lastwritten;
static std::map<ADDRINT, UINT64> readby;
typedef std::map<UINT64, UINT64> commItemType;
typedef std::map<UINT64, commItemType> commType;
static commType comm;
static std::map<UINT64, UINT64> combine;
static std::map<UINT64, std::map<UINT64, UINT64> > only_region;


static unsigned int lognextobject[MAX_THREADS] = { 0 };

/*static ADDRINT malloc_returnip[MAX_THREADS] = { 0 };
static ADDRINT malloc_size[MAX_THREADS] = { 0 };*/


int ln2(int value)
{
  int i, v = value;
  for (i = 0; i < 32; i++) {
    v >>= 1;
    if (v == 0)
      break;
  }
  assert(value == 1L << i);
  return i;
}



void unsafeThreadId(const THREADID threadid, const char * func, int line) {
  L();
  fprintf(stderr, "[PINCOMM] Got THREADID(%u) > MAX_THREADS(%u) at %s:%u !!\n", threadid, MAX_THREADS, func, line);
  U();
  exit(0);
}
inline void __safeThreadId(const THREADID threadid, const char * func, int line) {
  if ((unsigned)threadid >= MAX_THREADS) unsafeThreadId(threadid, func, line);
}
#define safeThreadId(threadid) __safeThreadId(threadid, __FUNCTION__, __LINE__)


inline void safeStackPtr(const THREADID threadid) {
  ;
}


VOID RecordEntry(THREADID threadid, UINT32 funcid, ADDRINT sp, UINT32 countFirst, ADDRINT returnIp);
VOID RecordReturn(THREADID threadid, UINT32 funcid, ADDRINT sp);

void checkFunc(const THREADID threadid, const UINT32 funcid, ADDRINT sp)
{
  if (!sp)
    return; /* check disabled (RecordEntry() called through us, don't recurse) */
  safeThreadId(threadid);
  if (callStack[threadid].empty()) {
    //printf("empty stack for %u, adding to back\n", funcid);
    RecordEntry(threadid, funcid, sp, 0, 0);
  } else if (sp > callStack[threadid].back().sp) {
    //ADDRINT oldsp=callStack[threadid].back().sp;
    while(!callStack[threadid].empty() && sp > callStack[threadid].back().sp)
      RecordReturn(threadid, 0, 0);
    if (callStack[threadid].empty())
      RecordEntry(threadid, funcid, sp, 0, 0);
    else if (funcid != callStack[threadid].back().funcid) {
      RecordReturn(threadid, 0, 0); /* pop current frame with wrong funcid */
      RecordEntry(threadid, funcid, sp, 0, 0); /* and replace with fresh frame with correct one */
    }
  } else if (sp > callStack[threadid].back().sp)
    printf("NONE sp %lx > %lx\n", (long)sp, (long)callStack[threadid].back().sp);
}


static void printStack(THREADID threadid)
{
  if (!callStack[threadid].empty()) {
    L();
    binstore_store_items(trace, "ci", 'S', threadid);
    for(threadStackType::iterator it = callStack[threadid].begin(); it != callStack[threadid].end(); ++it)
      binstore_store_items(trace, "(ii)", it->funcid, it->returnIp);
    binstore_store_end(trace);
    U();
  }
}


UINT64 makeRegion(THREADID threadid, stackItemType & item) {
  UINT64 mr = KnobRegionTime.Value() ? icount_tot / KnobRegionTime.Value() : item.mregion;
  assert(mr < MAX_MREGION);
  return (UINT64)item.dfuncid << 32 | mr << 10 | threadid;
}

VOID storeRegion(BINSTORE * trace, UINT64 region) {
  binstore_store_items(trace, "iii", (UINT32)(region & 0x3ff) /* threadid */,
    (UINT32)((region >> 10) & 0x3fffff) /* mreg */, (UINT32)(region >> 32) /* dfid */);
}

VOID setRegion(THREADID threadid) {
  if (!callStack[threadid].empty()) {
    region[threadid] = makeRegion(threadid, callStack[threadid].back());
    /*if (state == S_MEASURE) {
      L();
      binstore_store(trace, "ciiii", 'T', threadid, callStack[threadid].back().dfuncid, callStack[threadid].back().mregion, callStack[threadid].back().funcid);
      U();
    }*/
  } else
    region[threadid] = threadid;
}


VOID outputSelfAndParents(THREADID threadid) {
  if (!callStack[threadid].empty()) {
    /* make sure all parents (and self) have been output */
    unsigned int i = callStack[threadid].size() - 1; /* self */
    while(i && !callStack[threadid][i].output)
      --i;
    for(i = i + 1; i < callStack[threadid].size(); ++i) {
      stackItemType & item = callStack[threadid][i];
      binstore_store(trace, "ciiiil", 'E', threadid, item.funcid, item.dfuncid, item.returnIp, item.icounttot_start);
      item.output = 1;
    }
  }
}


VOID storeComm(UINT64 region) {
  /* collapse combined regions */
  for(std::map<UINT64, UINT64>::iterator it = comm[region].begin(); it != comm[region].end(); ++it) {
    if (combine.count(it->first)) {
      UINT64 parent = combine[it->first];
      while(combine.count(parent))
        parent = combine[parent];
      comm[region][parent] += it->second;
      comm[region][it->first] = 0;
    }
  }

  binstore_store_items(trace, "c", 'C');
  storeRegion(trace, region);
  for(std::map<UINT64, UINT64>::iterator it = comm[region].begin(); it != comm[region].end(); ++it) {
    if (it->first != region && it->second > 0) {
    binstore_store_items(trace, "(");
    storeRegion(trace, it->first);
    binstore_store_items(trace, "l)", it->second);
    }
  }
  binstore_store_end(trace);

  comm.erase(region);
}


void StateMeasureStart(string why)
{
  L();
  binstore_store(trace, "s", "START");
  fprintf(stdout, "[PINCOMM] Start: %s\n", why.c_str());
  fflush(stdout);
  state = S_MEASURE;
  for(callStackType::iterator it = callStack.begin(); it != callStack.end(); ++it) {
    icount[it->first] = 0;
    printStack(it->first);
    setRegion(it->first);
  }
  icount_tot = 0;
  icount_next = KnobRegionTime.Value();
  U();
}

void StateMeasureStop(string why)
{
  L();
  for(int tid = 0; tid < MAX_THREADS; ++tid)
    if (icount[tid])
      binstore_store(trace, "cil", 'I', tid, icount[tid]);

  for(callStackType::iterator it = callStack.begin(); it != callStack.end(); ++it) {
    while(!it->second.empty())
      RecordReturn(it->first /* threadid */, 0, 0);
  }

  for(commType::iterator it = comm.begin(); it != comm.end(); ++it)
    storeComm(it->first);

  binstore_store(trace, "s", "STOP");
  fprintf(stdout, "[PINCOMM] Stop: %s\n", why.c_str());
  fflush(stdout);
  U();
  state = S_INIT;
}

void StateMeasureEnd(BOOL theEnd)
{
  if (state == S_MEASURE)
    StateMeasureStop("ending");
  L();
  binstore_store(trace, "s", "END");
  fprintf(stdout, "[PINCOMM] End\n");
  fflush(stdout);
  state = S_DONE;
  U();
  if (!theEnd)
    PIN_Detach();
}


VOID LogMalloc(THREADID threadid, ADDRINT objectid, ADDRINT returnIp, ADDRINT address, ADDRINT size)
{
  outputSelfAndParents(threadid);
  binstore_store(trace, "ciiiii", 'M', threadid, objectid, returnIp, address, size);
}


VOID Magic(THREADID threadid, INT32 arg, INT32 arg1, INT32 arg2)
{
  int cmd = (arg & __PIN_CMD_MASK) >> __PIN_CMD_OFFSET, val = arg & __PIN_ID_MASK;

  if (KnobUseMagic) {
    /* program was compiled with Simics' MAGIC instruction,
       which does not explicitly set %eax. <arg> therefore
       contains garbage: ignore it */
    cmd = __PIN_MAGIC_CMD_NOARG;
    val = __PIN_MAGIC_SIMICS;
  }

  switch(cmd) {
    case __PIN_MAGIC_CMD_NOARG:
      switch (val) {
        case __PIN_MAGIC_START:
          StateMeasureStart("MAGIC start");
          break;
        case __PIN_MAGIC_STOP:
          StateMeasureStop("MAGIC stop");
          break;
        case __PIN_MAGIC_END:
          StateMeasureEnd(FALSE);
          break;
        case __PIN_MAGIC_SIMICS:
          if (KnobUseMagic) {
            switch(state) {
              case S_INIT:
                StateMeasureStart("SIMICS MAGIC");
                break;
              case S_MEASURE:
                StateMeasureStop("SIMICS MAGIC");
                StateMeasureEnd(FALSE);
                break;
              default:
                break;
            }
          }
          break;
      }
      break;
    case __PIN_MAGIC_MALLOC:
      // fprintf(stdout, "[PINCOMM] Next object is #%u\n", val); fflush(stdout);
      lognextobject[threadid] = val;
      break;
    case __PIN_MAGIC_MALLOCM:
      // fprintf(stdout, "[PINCOMM] Object #%u @ %x+%u\n", val, arg1, arg2); fflush(stdout);
      LogMalloc(threadid, val /* objectid */, callStack[threadid].back().funcid /* returnIp */, arg1 /* address */, arg2 /*size */);
      break;
    case __PIN_MAGIC_REGION:
      if (state == S_MEASURE) {
        L();
        outputSelfAndParents(threadid);
        binstore_store(trace, "ciil", 'G', threadid, val, icount[threadid]);
        safeThreadId(threadid);
        if (val > MAX_MREGION) {
          fprintf(stderr, "[PINCOMM] Got MREGION(%u) > MAX_MREGION(%u) !!\n", val, MAX_MREGION);
          exit(0);
        }
        callStack[threadid].back().mregion = val;
        setRegion(threadid);
        U();
      }
      break;
    case __PIN_MAGIC_ZONE_ENTER:
      if (KnobZone.Value() == val && state != S_MEASURE)
        StateMeasureStart("ZONE ENTER");
      break;
    case __PIN_MAGIC_ZONE_EXIT:
      if (KnobZone.Value() == val && state == S_MEASURE) {
        StateMeasureStop("ZONE EXIT");
        StateMeasureEnd(FALSE);
      }
      break;
    default:
      fprintf(stdout, "[PINCOMM] Unknown MAGIC %u\n", cmd);
      fflush(stdout);
      break;
  }
}

VOID CountInstructions(THREADID threadid, INT32 count) {
  safeThreadId(threadid);
  icount[threadid] += count;
  icount_tot += count;
}


// Print a memory read record
VOID RecordMemRead(THREADID threadid, UINT32 funcid, ADDRINT sp, ADDRINT addr, ADDRINT size)
{
  if (state != S_MEASURE) return;
  checkFunc(threadid, funcid, sp);
  L();
  //binstore_store(trace, "ciii", 'R', threadid, addr, size);

  if (icount_tot > icount_next) {
    icount_next += KnobRegionTime.Value();
    for(callStackType::iterator it = callStack.begin(); it != callStack.end(); ++it) {
      setRegion(it->first);
    }
  }

  //binstore_store(trace, "clli", 'C', lastwritten[addr], region[threadid], size);
  int commBytes = 0, isComm = false, commBytes_cache = 0, isComm_cache = false;
  for(ADDRINT a = addr >> memgran_bits; a <= (addr + size - 1) >> memgran_bits; ++a) {
    ADDRINT s = 1 << memgran_bits;
    if (a == addr >> memgran_bits)
      s -= (addr - (a << memgran_bits));
    if (a == (addr + size - 1) >> memgran_bits)
      s -= ((a + 1) << memgran_bits) - (addr + size);

    if (KnobRegionOnly.Value()) {
      UINT64 src = (lastwritten[a] >> 10) & 0xff,
             dst = (region[threadid] >> 10) & 0xff;
      if (only_region.count(src) == 0)      only_region[src] = std::map<UINT64, UINT64>();
      if (only_region[src].count(dst) == 0) only_region[src][dst] = 0;
      only_region[src][dst] += s;
    }

    comm[region[threadid]][lastwritten[a]] += s;
    if (s && lastwritten[a]
        && threadid != (UINT32)(lastwritten[a] & 0x3ff))
    {
      isComm = TRUE;
      commBytes += s;
      if (!(readby[a] & (1 << threadid))) {
        isComm_cache = true;
        commBytes_cache += 1 << memgran_bits;
      }
    }
    readby[a] |= 1 << threadid;
  }
  if (isComm) ++icount_read;
  if (isComm_cache) ++icount_read_cache;
  bcount_read += commBytes;
  bcount_read_cache += commBytes_cache;
  U();
}

// Print a memory write record
VOID RecordMemWrite(THREADID threadid, UINT32 funcid, ADDRINT sp, ADDRINT addr, ADDRINT size)
{
  if (state != S_MEASURE) return;
  checkFunc(threadid, funcid, sp);
  L();
  //binstore_store(trace, "ciii", 'W', threadid, addr, size);

  for(ADDRINT a = addr >> memgran_bits; a <= (addr + size - 1) >> memgran_bits; ++a) {
    lastwritten[a] = region[threadid];
    readby[a] = 0;
  }
  U();
}


// Print a function entry record
VOID RecordEntry(THREADID threadid, UINT32 funcid, ADDRINT sp, UINT32 countFirst, ADDRINT returnIp)
{
  assert(funcid);
  safeThreadId(threadid);
  UINT32 dfid = ++dfuncid[threadid];
  safeStackPtr(threadid);
  UINT32 mregion = callStack[threadid].empty() ? 0 : callStack[threadid].back().mregion;
  callStack[threadid].push_back(stackItemType());
  callStack[threadid].back().funcid = funcid;
  callStack[threadid].back().sp = sp;
  callStack[threadid].back().returnIp = returnIp;
  callStack[threadid].back().mregion = mregion;
  callStack[threadid].back().dfuncid = dfid;
  callStack[threadid].back().icount_start = icount[threadid] - countFirst;
  callStack[threadid].back().icounttot_start = icount_tot;
  callStack[threadid].back().output = state == S_MEASURE ? 0 : 1;
  setRegion(threadid);
}


// Print a return record
VOID RecordReturn(THREADID threadid, UINT32 funcid, ADDRINT sp)
{
  if (sp && !callStack[threadid].empty() && sp < callStack[threadid].back().sp)
    return;
  checkFunc(threadid, funcid, sp);
  if (!callStack[threadid].empty()) {
    if (state == S_MEASURE) {
      L();

      if (icount[threadid] - callStack[threadid].back().icount_start < KnobMinLen.Value()
        && callStack[threadid].size() > 1) {
        /* function too short, merge into parent */
        UINT64 parent = makeRegion(threadid, callStack[threadid][callStack[threadid].size() - 2]);
        combine[region[threadid]] = parent;
        for(std::map<UINT64, UINT64>::iterator it = comm[region[threadid]].begin(); it != comm[region[threadid]].end(); ++it) {
          comm[parent][it->first] += it->second;
        }

        /* frame was opened ('E' emited), make sure we close it (emit 'X') */
        if (callStack[threadid].back().output)
          binstore_store(trace, "cili", 'X', threadid, icount[threadid], 1);
      } else {

        outputSelfAndParents(threadid);
        storeComm(region[threadid]);
        binstore_store(trace, "cili", 'X', threadid, icount[threadid], 0);
      }
      comm.erase(region[threadid]);

      U();
    }
    callStack[threadid].pop_back();
  }
  setRegion(threadid);
}


#if 0
VOID Malloc_Before(ADDRINT size, THREADID threadid, ADDRINT returnIp)
{
  safeThreadId(threadid);
  malloc_size[threadid] = size;
  malloc_returnip[threadid] = threadid;
}

VOID Malloc_After(ADDRINT address, THREADID threadid)
{
  safeThreadId(threadid);
  ADDRINT size = malloc_size[threadid];
  ADDRINT returnIp = malloc_returnip[threadid];
#else
VOID Malloc(ADDRINT size, ADDRINT address, THREADID threadid, ADDRINT returnIp)
{
#endif
  L();
  if (lognextobject[threadid]) {
    // fprintf(stdout, "[PINCOMM] Got object #%u\n", lognextobject[threadid]); fflush(stdout);
    if (state != S_MEASURE)
      printStack(threadid);
    LogMalloc(threadid, lognextobject[threadid], returnIp, address, size);
    lognextobject[threadid] = 0;
  } else
    LogMalloc(threadid, 0, returnIp, address, size);
//printf("malloc: %x %d\n", address, size);
  U();
}


VOID Free(ADDRINT address, THREADID threadid, ADDRINT returnIp)
{
  L();
  outputSelfAndParents(threadid);
  binstore_store(trace, "cii", 'N', threadid, address);
//printf("free: %x\n", address);
  U();
}





VOID * Jit_Malloc_IA32( CONTEXT * context, AFUNPTR orgFuncptr, size_t size, THREADID threadid)
{
    VOID * ret;

    PIN_CallApplicationFunction( context, PIN_ThreadId(),
                                 CALLINGSTD_DEFAULT, orgFuncptr,
                                 PIN_PARG(void *), &ret,
                                 PIN_PARG(size_t), size,
                                 PIN_PARG_END() );

//printf("malloc(%d) returns %p\n", size, ret);
    Malloc(size, (ADDRINT)ret, threadid, 0);
    return ret;
}

/* ===================================================================== */

VOID Jit_Free_IA32( CONTEXT * context, AFUNPTR orgFuncptr, void * ptr, THREADID threadid)
{
    PIN_CallApplicationFunction( context, PIN_ThreadId(),
                                 CALLINGSTD_DEFAULT, orgFuncptr,
                                 PIN_PARG(void),
                                 PIN_PARG(void *), ptr,
                                 PIN_PARG_END() );

//printf("free(%p)\n", ptr);
    Free((ADDRINT)ptr, threadid, 0);
}

VOID ImageLoad(IMG img, VOID *v)
{
    RTN mallocRtn = RTN_FindByName(img, "malloc");
    if (RTN_Valid(mallocRtn))
    {
        PROTO proto_malloc = PROTO_Allocate( PIN_PARG(void *), CALLINGSTD_DEFAULT,
                                             "malloc", PIN_PARG(size_t), PIN_PARG_END() );


        RTN_ReplaceSignature(
            mallocRtn, AFUNPTR( Jit_Malloc_IA32 ),
            IARG_PROTOTYPE, proto_malloc,
            IARG_CONTEXT,
            IARG_ORIG_FUNCPTR,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
            IARG_THREAD_ID,
            IARG_END);
        //TraceFile << "Replaced malloc() in:"  << IMG_Name(img) << endl;
    }

    RTN freeRtn = RTN_FindByName(img, "free");
    if (RTN_Valid(freeRtn))
    {
        PROTO proto_free = PROTO_Allocate( PIN_PARG(void), CALLINGSTD_DEFAULT,
                                           "free", PIN_PARG(void *), PIN_PARG_END() );


        RTN_ReplaceSignature(
            freeRtn, AFUNPTR( Jit_Free_IA32 ),
            IARG_PROTOTYPE, proto_free,
            IARG_CONTEXT,
            IARG_ORIG_FUNCPTR,
            IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
            IARG_THREAD_ID,
            IARG_END);
        //TraceFile << "Replaced free() in:"  << IMG_Name(img) << endl;
    }
}








VOID Trace(TRACE trace, VOID *v)
{
  for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl))
  {
    BBL_InsertCall(bbl, IPOINT_BEFORE, (AFUNPTR)CountInstructions, IARG_THREAD_ID, IARG_UINT32, BBL_NumIns(bbl), IARG_END);
  }
}


VOID Routine(RTN rtn, VOID *v)
{
  UINT32 funcid = RTN_Address(rtn);

  INT32 line; string fileName;
  PIN_GetSourceLocation(RTN_Address(rtn), NULL, &line, &fileName);
  binstore_store(trace, "cisssi", 'F', funcid, IMG_Name(SEC_Img(RTN_Sec(rtn))).c_str(), RTN_Name(rtn).c_str(), fileName.c_str(), line);

  RTN_Open(rtn);

  /*if (RTN_Name(rtn) == "malloc")
    RTN_InsertCall(rtn, IPOINT_AFTER, (AFUNPTR)Malloc, IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_FUNCRET_EXITPOINT_VALUE, IARG_THREAD_ID, IARG_RETURN_IP, IARG_END);
  else*/
  /*if (RTN_Name(rtn) == "_int_malloc") {
    RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)Malloc_Before, IARG_FUNCARG_CALLSITE_VALUE, 1, IARG_THREAD_ID, IARG_RETURN_IP, IARG_END);
    RTN_InsertCall(rtn, IPOINT_AFTER, (AFUNPTR)Malloc_After, IARG_FUNCRET_EXITPOINT_VALUE, IARG_THREAD_ID, IARG_END);
  }
  else if (RTN_Name(rtn) == "_int_free")
    RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)Free, IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_THREAD_ID, IARG_RETURN_IP, IARG_END);
    */

  RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)RecordEntry, IARG_THREAD_ID, IARG_UINT32, funcid, IARG_REG_VALUE, REG_STACK_PTR, IARG_UINT32, 0/*BBL_NumIns(RTN_BblHead(rtn))*/, IARG_RETURN_IP, IARG_END);
  for (INS ins = RTN_InsHead(rtn); INS_Valid(ins); ins = INS_Next(ins))
  {
    if (!KnobIgnoreComm) {
      if (INS_IsMemoryRead(ins)) {
        INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)RecordMemRead, IARG_THREAD_ID, IARG_UINT32, funcid, IARG_REG_VALUE, REG_STACK_PTR, IARG_MEMORYREAD_EA, IARG_MEMORYREAD_SIZE, IARG_END);
        if (INS_HasMemoryRead2(ins))
          INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)RecordMemRead, IARG_THREAD_ID, IARG_UINT32, funcid, IARG_REG_VALUE, REG_STACK_PTR, IARG_MEMORYREAD2_EA, IARG_MEMORYREAD_SIZE, IARG_END);
      }
      if (INS_IsMemoryWrite(ins))
        INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)RecordMemWrite, IARG_THREAD_ID, IARG_UINT32, funcid, IARG_REG_VALUE, REG_STACK_PTR, IARG_MEMORYWRITE_EA, IARG_MEMORYWRITE_SIZE, IARG_END);
    }
    if (INS_IsRet(ins))
      INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)RecordReturn, IARG_THREAD_ID, IARG_UINT32, funcid, IARG_REG_VALUE, REG_STACK_PTR, IARG_END);
    if (INS_Disassemble(ins) == "xchg bx, bx") {
      /* SIMICS Magic Instruction */
      INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)Magic, IARG_THREAD_ID, IARG_REG_VALUE, REG_EAX, IARG_REG_VALUE, REG_ECX, IARG_REG_VALUE, REG_EDX, IARG_END);
    }
    if (INS_IsCall(ins)) {
      INT32 line; string fileName;
      PIN_GetSourceLocation(INS_Address(ins), NULL, &line, &fileName);
      if (line)
        binstore_store(trace, "ciisi", 'A', INS_NextAddress(ins), RTN_Address(INS_Rtn(ins)), fileName.c_str(), line);
    }
  }

  RTN_Close(rtn);
}


VOID TheEnd()
{
  if (state == S_MEASURE)
    StateMeasureEnd(TRUE);
  binstore_close(trace);
  if (KnobRegionOnly.Value()) {
    FILE *fp = fopen(KnobCsvOutputFile.Value().c_str(), "w");
    for(std::map<UINT64, std::map<UINT64, UINT64> >::iterator it = only_region.begin(); it != only_region.end(); ++it)
      for(std::map<UINT64, UINT64>::iterator jt = it->second.begin(); jt != it->second.end(); ++jt)
        fprintf(fp, "%"PRIu64",%"PRIu64",%"PRIu64"\n", it->first, jt->first, jt->second);
    fclose(fp);
  }
}

VOID Fini(INT32 code, VOID *v)
{
  TheEnd();
}

VOID Detach(VOID *v)
{
  TheEnd();
}


int main(int argc, char *argv[])
{
  PIN_InitSymbols();

  PIN_Init(argc, argv);

  if (KnobOutputCmd.Value() != "")
    trace = binstore_open(KnobOutputCmd.Value().c_str(), "wp");
  else
    trace = binstore_open(KnobOutputFile.Value().c_str(), "w");
  if (!trace) {
    fprintf(stderr, "[PINCOMM] Cannot open trace output file!\n");
    exit(-1);
  }

  memgran_bits = ln2(KnobMemGran.Value());


  IMG_AddInstrumentFunction(ImageLoad, 0);

  RTN_AddInstrumentFunction(Routine, 0);
  TRACE_AddInstrumentFunction(Trace, 0);
  PIN_AddFiniFunction(Fini, 0);
  PIN_AddDetachFunction(Detach, 0);

  state = S_INIT;
  if (!KnobUseMagic && !KnobZone.Value())
    StateMeasureStart("program start");

  // Never returns
  PIN_StartProgram();

  return 0;
}
