//===-- Executor.cpp ------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Common.h"

#include "Executor.h"
 
#include "Context.h"
#include "CoreStats.h"
#include "ExternalDispatcher.h"
#include "ImpliedValue.h"
#include "Memory.h"
#include "MemoryManager.h"
#include "PTree.h"
#include "Searcher.h"
#include "SeedInfo.h"
#include "SpecialFunctionHandler.h"
#include "StatsTracker.h"
#include "TimingSolver.h"
#include "UserSearcher.h"
#include "../Solver/SolverStats.h"
#include "klee/BasicCheckers.h"

#include "klee/ExecutionState.h"
#include "klee/Expr.h"
#include "klee/Interpreter.h"
#include "klee/TimerStatIncrementer.h"
#include "klee/util/Assignment.h"
#include "klee/util/ExprPPrinter.h"
#include "klee/util/ExprUtil.h"
#include "klee/util/GetElementPtrTypeIterator.h"
#include "klee/Config/Version.h"
#include "klee/Internal/ADT/KTest.h"
#include "klee/Internal/ADT/RNG.h"
#include "klee/Internal/Module/Cell.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/KModule.h"
#include "klee/Internal/Support/FloatEvaluation.h"
#include "klee/Internal/System/Time.h"

#include "llvm/Attributes.h"
#include "llvm/BasicBlock.h"
#include "llvm/Constants.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/IntrinsicInst.h"
#if LLVM_VERSION_CODE >= LLVM_VERSION(2, 7)
#include "llvm/LLVMContext.h"
#endif
#include "llvm/Module.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#if LLVM_VERSION_CODE < LLVM_VERSION(2, 9)
#include "llvm/System/Process.h"
#else
#include "llvm/Support/Process.h"
#endif
#include "llvm/Target/TargetData.h"

#include <cassert>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>

#include <sys/mman.h>

#include <errno.h>
#include <cxxabi.h>

using namespace llvm;
using namespace klee;

namespace {
  cl::opt<bool>
  DumpStatesOnHalt("dump-states-on-halt",
                   cl::init(true));
 
  cl::opt<bool>
  NoPreferCex("no-prefer-cex",
              cl::init(false));
 
  cl::opt<bool>
  UseAsmAddresses("use-asm-addresses",
                  cl::init(false));
 
  cl::opt<bool>
  RandomizeFork("randomize-fork",
                cl::init(false));
 
  cl::opt<bool>
  AllowExternalSymCalls("allow-external-sym-calls",
                        cl::init(false));

  cl::opt<bool>
  DebugPrintInstructions("debug-print-instructions", 
                         cl::desc("Print instructions during execution."));

  cl::opt<bool>
  DebugCheckForImpliedValues("debug-check-for-implied-values");


  cl::opt<bool>
  SimplifySymIndices("simplify-sym-indices",
                     cl::init(false));

  cl::opt<unsigned>
  MaxSymArraySize("max-sym-array-size",
                  cl::init(0));

  cl::opt<bool>
  DebugValidateSolver("debug-validate-solver",
		      cl::init(false));

  cl::opt<bool>
  SuppressExternalWarnings("suppress-external-warnings");

  cl::opt<bool>
  AllExternalWarnings("all-external-warnings");

  cl::opt<bool>
  OnlyOutputStatesCoveringNew("only-output-states-covering-new",
                              cl::init(false));

  cl::opt<bool>
  AlwaysOutputSeeds("always-output-seeds",
                              cl::init(true));

  cl::opt<bool>
  UseFastCexSolver("use-fast-cex-solver",
		   cl::init(false));

  cl::opt<bool>
  UseIndependentSolver("use-independent-solver",
                       cl::init(true),
		       cl::desc("Use constraint independence"));

  cl::opt<bool>
  EmitAllErrors("emit-all-errors",
                cl::init(false),
                cl::desc("Generate tests cases for all errors "
                         "(default=one per (error,instruction) pair)"));

  cl::opt<bool>
  UseCexCache("use-cex-cache",
              cl::init(true),
	      cl::desc("Use counterexample caching"));

  cl::opt<bool>
  UseQueryPCLog("use-query-pc-log",
                cl::init(false));
  
  cl::opt<bool>
  UseSTPQueryPCLog("use-stp-query-pc-log",
                   cl::init(false));

  cl::opt<bool>
  NoExternals("no-externals", 
           cl::desc("Do not allow external functin calls"));

  cl::opt<bool>
  UseCache("use-cache",
	   cl::init(true),
	   cl::desc("Use validity caching"));

  cl::opt<bool>
  OnlyReplaySeeds("only-replay-seeds", 
                  cl::desc("Discard states that do not have a seed."));
 
  cl::opt<bool>
  OnlySeed("only-seed", 
           cl::desc("Stop execution after seeding is done without doing regular search."));
 
  cl::opt<bool>
  AllowSeedExtension("allow-seed-extension", 
                     cl::desc("Allow extra (unbound) values to become symbolic during seeding."));
 
  cl::opt<bool>
  ZeroSeedExtension("zero-seed-extension");
 
  cl::opt<bool>
  AllowSeedTruncation("allow-seed-truncation", 
                      cl::desc("Allow smaller buffers than in seeds."));
 
  cl::opt<bool>
  NamedSeedMatching("named-seed-matching",
                    cl::desc("Use names to match symbolic objects to inputs."));

  cl::opt<double>
  MaxStaticForkPct("max-static-fork-pct", cl::init(1.));
  cl::opt<double>
  MaxStaticSolvePct("max-static-solve-pct", cl::init(1.));
  cl::opt<double>
  MaxStaticCPForkPct("max-static-cpfork-pct", cl::init(1.));
  cl::opt<double>
  MaxStaticCPSolvePct("max-static-cpsolve-pct", cl::init(1.));
  
  cl::opt<double>
  SeedTime("seed-time",
           cl::desc("Amount of time to dedicate to seeds, before normal search (default=0 (off))"),
           cl::init(0));
  
  cl::opt<unsigned int>
  StopAfterNInstructions("stop-after-n-instructions",
                         cl::desc("Stop execution after specified number of instructions (0=off)"),
                         cl::init(0));
  
  cl::opt<unsigned>
  MaxForks("max-forks",
           cl::desc("Only fork this many times (-1=off)"),
           cl::init(~0u));
  
  cl::opt<unsigned>
  MaxDepth("max-depth",
           cl::desc("Only allow this many symbolic branches (0=off)"),
           cl::init(0));
  
  cl::opt<unsigned>
  MaxMemory("max-memory",
            cl::desc("Refuse to fork when more above this about of memory (in MB, 0=off)"),
            cl::init(0));

  cl::opt<bool>
  MaxMemoryInhibit("max-memory-inhibit",
            cl::desc("Inhibit forking at memory cap (vs. random terminate)"),
            cl::init(true));

  cl::opt<bool>
  UseForkedSTP("use-forked-stp", 
                 cl::desc("Run STP in forked process"));

  cl::opt<bool>
  STPOptimizeDivides("stp-optimize-divides", 
                 cl::desc("Optimize constant divides into add/shift/multiplies before passing to STP"),
                 cl::init(true));
}

cl::opt<std::string>
UseOneChecker("use-one-checker", 
               cl::desc("Specify only one checker for KLEE. Can be Assert, OpenClose or Lock"),
               cl::init(""));  

cl::opt<bool>
UsePathSlicer("use-path-slicer", 
               cl::desc("Path slicer to slice out irrelevant branches for the checker targets"),
               cl::init(false));

cl::opt<bool>
MarkPrunedOnly("mark-pruned-only", 
               cl::desc("Mark pruned states by path slicer only, do not actually prune it."),
               cl::init(false));

extern cl::opt<double> MaxInstructionTime;
extern cl::opt<double> MaxSTPTime;

namespace klee {
  RNG theRNG;
}

#include "path-slicer/path-slicer.h"
using namespace tern;
PathSlicer *pathSlicer;

Solver *constructSolverChain(STPSolver *stpSolver,
                             std::string queryLogPath,
                             std::string stpQueryLogPath,
                             std::string queryPCLogPath,
                             std::string stpQueryPCLogPath) {
  Solver *solver = stpSolver;

  if (UseSTPQueryPCLog)
    solver = createPCLoggingSolver(solver, 
                                   stpQueryLogPath);

  if (UseFastCexSolver)
    solver = createFastCexSolver(solver);

  if (UseCexCache)
    solver = createCexCachingSolver(solver);

  if (UseCache)
    solver = createCachingSolver(solver);

  if (UseIndependentSolver)
    solver = createIndependentSolver(solver);

  if (DebugValidateSolver)
    solver = createValidatingSolver(solver, stpSolver);

  if (UseQueryPCLog)
    solver = createPCLoggingSolver(solver, 
                                   queryPCLogPath);
  
  return solver;
}

Executor::Executor(const InterpreterOptions &opts,
                   InterpreterHandler *ih) 
  : Interpreter(opts),
    kmodule(0),
    interpreterHandler(ih),
    searcher(0),
    externalDispatcher(new ExternalDispatcher()),
    statsTracker(0),
    pathWriter(0),
    symPathWriter(0),
    specialFunctionHandler(0),
    processTree(0),
    replayOut(0),
    replayPath(0),    
    usingSeeds(0),
    atMemoryLimit(false),
    inhibitForking(false),
    haltExecution(false),
    ivcEnabled(false),
    stpTimeout(MaxSTPTime != 0 && MaxInstructionTime != 0
      ? std::min(MaxSTPTime,MaxInstructionTime)
      : std::max(MaxSTPTime,MaxInstructionTime)) {
  STPSolver *stpSolver = new STPSolver(UseForkedSTP, STPOptimizeDivides);
  Solver *solver = 
    constructSolverChain(stpSolver,
                         interpreterHandler->getOutputFilename("queries.qlog"),
                         interpreterHandler->getOutputFilename("stp-queries.qlog"),
                         interpreterHandler->getOutputFilename("queries.pc"),
                         interpreterHandler->getOutputFilename("stp-queries.pc"));
  
  this->solver = new TimingSolver(solver, stpSolver);

  memory = new MemoryManager();
}


const Module *Executor::setModule(llvm::Module *module, 
                                  const ModuleOptions &opts) {
  assert(!kmodule && module && "can only register one module"); // XXX gross
  
  kmodule = new KModule(module);

  // Initialize the context.
  TargetData *TD = kmodule->targetData;
  Context::initialize(TD->isLittleEndian(),
                      (Expr::Width) TD->getPointerSizeInBits());

  specialFunctionHandler = new SpecialFunctionHandler(*this);

  specialFunctionHandler->prepare();
  kmodule->prepare(opts, interpreterHandler);
  specialFunctionHandler->bind();

  if (StatsTracker::useStatistics()) {
    statsTracker = 
      new StatsTracker(*this,
                       interpreterHandler->getOutputFilename("assembly.ll"),
                       userSearcherRequiresMD2U());
  }

  if (UsePathSlicer)
    pathSlicer->initKModule(kmodule);
  return module;
}

Executor::~Executor() {
  if (UsePathSlicer)
    delete pathSlicer;
  delete memory;
  delete externalDispatcher;
  if (processTree)
    delete processTree;
  if (specialFunctionHandler)
    delete specialFunctionHandler;
  if (statsTracker)
    delete statsTracker;
  delete solver;
  delete kmodule;
}

/***/

void Executor::initializeGlobalObject(ExecutionState &state, ObjectState *os,
                                      const Constant *c, 
                                      unsigned offset) {
  TargetData *targetData = kmodule->targetData;
  if (const ConstantVector *cp = dyn_cast<ConstantVector>(c)) {
    unsigned elementSize =
      targetData->getTypeStoreSize(cp->getType()->getElementType());
    for (unsigned i=0, e=cp->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, cp->getOperand(i), 
			     offset + i*elementSize);
  } else if (isa<ConstantAggregateZero>(c)) {
    unsigned i, size = targetData->getTypeStoreSize(c->getType());
    for (i=0; i<size; i++)
      os->write8(offset+i, (uint8_t) 0);
  } else if (const ConstantArray *ca = dyn_cast<ConstantArray>(c)) {
    unsigned elementSize =
      targetData->getTypeStoreSize(ca->getType()->getElementType());
    for (unsigned i=0, e=ca->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, ca->getOperand(i), 
			     offset + i*elementSize);
  } else if (const ConstantStruct *cs = dyn_cast<ConstantStruct>(c)) {
    const StructLayout *sl =
      targetData->getStructLayout(cast<StructType>(cs->getType()));
    for (unsigned i=0, e=cs->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, cs->getOperand(i), 
			     offset + sl->getElementOffset(i));
  } else {
    unsigned StoreBits = targetData->getTypeStoreSizeInBits(c->getType());
    ref<ConstantExpr> C = evalConstant(c);

    // Extend the constant if necessary;
    assert(StoreBits >= C->getWidth() && "Invalid store size!");
    if (StoreBits > C->getWidth())
      C = C->ZExt(StoreBits);

    os->write(offset, C);
  }
}

MemoryObject * Executor::addExternalObject(ExecutionState &state, 
                                           void *addr, unsigned size, 
                                           bool isReadOnly) {
  MemoryObject *mo = memory->allocateFixed((uint64_t) (unsigned long) addr, 
                                           size, 0);
  ObjectState *os = bindObjectInState(state, NULL, mo, false);
  for(unsigned i = 0; i < size; i++)
    os->write8(i, ((uint8_t*)addr)[i]);
  if(isReadOnly)
    os->setReadOnly(true);  
  return mo;
}


extern void *__dso_handle __attribute__ ((__weak__));

void Executor::initializeGlobals(ExecutionState &state) {
  Module *m = kmodule->module;

  if (m->getModuleInlineAsm() != "")
    klee_warning("executable has module level assembly (ignoring)");

  assert(m->lib_begin() == m->lib_end() &&
         "XXX do not support dependent libraries");

  // represent function globals using the address of the actual llvm function
  // object. given that we use malloc to allocate memory in states this also
  // ensures that we won't conflict. we don't need to allocate a memory object
  // since reading/writing via a function pointer is unsupported anyway.
  for (Module::iterator i = m->begin(), ie = m->end(); i != ie; ++i) {
    Function *f = i;
    ref<ConstantExpr> addr(0);

    // If the symbol has external weak linkage then it is implicitly
    // not defined in this module; if it isn't resolvable then it
    // should be null.
    if (f->hasExternalWeakLinkage() && 
        !externalDispatcher->resolveSymbol(f->getName())) {
      addr = Expr::createPointer(0);
    } else {
      addr = Expr::createPointer((unsigned long) (void*) f);
      legalFunctions.insert((uint64_t) (unsigned long) (void*) f);
    }
    
    globalAddresses.insert(std::make_pair(f, addr));
  }

  // Disabled, we don't want to promote use of live externals.
#ifdef HAVE_CTYPE_EXTERNALS
#ifndef WINDOWS
#ifndef DARWIN
  /* From /usr/include/errno.h: it [errno] is a per-thread variable. */
  int *errno_addr = __errno_location();
  addExternalObject(state, (void *)errno_addr, sizeof *errno_addr, false);

  /* from /usr/include/ctype.h:
       These point into arrays of 384, so they can be indexed by any `unsigned
       char' value [0,255]; by EOF (-1); or by any `signed char' value
       [-128,-1).  ISO C requires that the ctype functions work for `unsigned */
  const uint16_t **addr = __ctype_b_loc();
  addExternalObject(state, (void *)(*addr-128), 
                    384 * sizeof **addr, true);
  addExternalObject(state, addr, sizeof(*addr), true);
    
  const int32_t **lower_addr = __ctype_tolower_loc();
  addExternalObject(state, (void *)(*lower_addr-128), 
                    384 * sizeof **lower_addr, true);
  addExternalObject(state, lower_addr, sizeof(*lower_addr), true);
  
  const int32_t **upper_addr = __ctype_toupper_loc();
  addExternalObject(state, (void *)(*upper_addr-128), 
                    384 * sizeof **upper_addr, true);
  addExternalObject(state, upper_addr, sizeof(*upper_addr), true);
#endif
#endif
#endif

  // allocate and initialize globals, done in two passes since we may
  // need address of a global in order to initialize some other one.

  // allocate memory objects for all globals
  for (Module::const_global_iterator i = m->global_begin(),
         e = m->global_end();
       i != e; ++i) {
    if (i->isDeclaration()) {
      // FIXME: We have no general way of handling unknown external
      // symbols. If we really cared about making external stuff work
      // better we could support user definition, or use the EXE style
      // hack where we check the object file information.

      LLVM_TYPE_Q Type *ty = i->getType()->getElementType();
      uint64_t size = kmodule->targetData->getTypeStoreSize(ty);

      // XXX - DWD - hardcode some things until we decide how to fix.
#ifndef WINDOWS
      if (i->getName() == "_ZTVN10__cxxabiv117__class_type_infoE") {
        size = 0x2C;
      } else if (i->getName() == "_ZTVN10__cxxabiv120__si_class_type_infoE") {
        size = 0x2C;
      } else if (i->getName() == "_ZTVN10__cxxabiv121__vmi_class_type_infoE") {
        size = 0x2C;
      }
#endif

      if (size == 0) {
        llvm::errs() << "Unable to find size for global variable: " 
                     << i->getName() 
                     << " (use will result in out of bounds access)\n";
      }

      MemoryObject *mo = memory->allocate(size, false, true, i);
      ObjectState *os = bindObjectInState(state, NULL, mo, false);
      globalObjects.insert(std::make_pair(i, mo));
      globalAddresses.insert(std::make_pair(i, mo->getBaseExpr()));

      // Program already running = object already initialized.  Read
      // concrete value and write it to our copy.
      if (size) {
        void *addr;
        if (i->getName() == "__dso_handle") {
          addr = &__dso_handle; // wtf ?
        } else {
          addr = externalDispatcher->resolveSymbol(i->getName());
        }
        if (!addr)
          klee_error("unable to load symbol(%s) while initializing globals.", 
                     i->getName().data());

        for (unsigned offset=0; offset<mo->size; offset++)
          os->write8(offset, ((unsigned char*)addr)[offset]);
      }
    } else {
      LLVM_TYPE_Q Type *ty = i->getType()->getElementType();
      uint64_t size = kmodule->targetData->getTypeStoreSize(ty);
      MemoryObject *mo = 0;

      if (UseAsmAddresses && i->getName()[0]=='\01') {
        char *end;
        uint64_t address = ::strtoll(i->getName().str().c_str()+1, &end, 0);

        if (end && *end == '\0') {
          klee_message("NOTE: allocated global at asm specified address: %#08llx"
                       " (%llu bytes)",
                       (long long) address, (unsigned long long) size);
          mo = memory->allocateFixed(address, size, &*i);
          mo->isUserSpecified = true; // XXX hack;
        }
      }

      if (!mo)
        mo = memory->allocate(size, false, true, &*i);
      mo->setName(i->getName());
      assert(mo && "out of memory");
      ObjectState *os = bindObjectInState(state, NULL, mo, false);
      globalObjects.insert(std::make_pair(i, mo));
      globalAddresses.insert(std::make_pair(i, mo->getBaseExpr()));

      if (!i->hasInitializer())
          os->initializeToRandom();
    }
  }
  
  // link aliases to their definitions (if bound)
  for (Module::alias_iterator i = m->alias_begin(), ie = m->alias_end(); 
       i != ie; ++i) {
    // Map the alias to its aliasee's address. This works because we have
    // addresses for everything, even undefined functions. 
    globalAddresses.insert(std::make_pair(i, evalConstant(i->getAliasee())));
  }

  // once all objects are allocated, do the actual initialization
  for (Module::const_global_iterator i = m->global_begin(),
         e = m->global_end();
       i != e; ++i) {
    if (i->hasInitializer()) {
      MemoryObject *mo = globalObjects.find(i)->second;
      const ObjectState *os = state.addressSpace.findObject(mo);
      assert(os);
      ObjectState *wos = state.addressSpace.getWriteable(mo, os);
      
      initializeGlobalObject(state, wos, i->getInitializer(), 0);
      // if(i->isConstant()) os->setReadOnly(true);
    }
  }
}

void Executor::branch(ExecutionState &state, 
                      const std::vector< ref<Expr> > &conditions,
                      std::vector<ExecutionState*> &result) {
  TimerStatIncrementer timer(stats::forkTime);
  unsigned N = conditions.size();
  assert(N);

  stats::forks += N-1;

  // XXX do proper balance or keep random?
  result.push_back(&state);
  for (unsigned i=1; i<N; ++i) {
    ExecutionState *es = result[theRNG.getInt32() % i];
    ExecutionState *ns = es->branch();
    if (UsePathSlicer) {
      pathSlicer->copyTrace((void *)ns, (void *)es);
      size_t dynInstrIdx = pathSlicer->getLatestBrOrExtCallIdx((void *)es);
      brTree.addBranch(es, dynInstrIdx, ns);
    }
    addedStates.insert(ns);
    result.push_back(ns);
    es->ptreeNode->data = 0;
    std::pair<PTree::Node*,PTree::Node*> res = 
      processTree->split(es->ptreeNode, ns, es);
    ns->ptreeNode = res.first;
    es->ptreeNode = res.second;
  }

  // If necessary redistribute seeds to match conditions, killing
  // states if necessary due to OnlyReplaySeeds (inefficient but
  // simple).
  
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
    seedMap.find(&state);
  if (it != seedMap.end()) {
    std::vector<SeedInfo> seeds = it->second;
    seedMap.erase(it);

    // Assume each seed only satisfies one condition (necessarily true
    // when conditions are mutually exclusive and their conjunction is
    // a tautology).
    for (std::vector<SeedInfo>::iterator siit = seeds.begin(), 
           siie = seeds.end(); siit != siie; ++siit) {
      unsigned i;
      for (i=0; i<N; ++i) {
        ref<ConstantExpr> res;
        bool success = 
          solver->getValue(state, siit->assignment.evaluate(conditions[i]), 
                           res);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;
        if (res->isTrue())
          break;
      }
      
      // If we didn't find a satisfying condition randomly pick one
      // (the seed will be patched).
      if (i==N)
        i = theRNG.getInt32() % N;

      seedMap[result[i]].push_back(*siit);
    }

    if (OnlyReplaySeeds) {
      for (unsigned i=0; i<N; ++i) {
        if (!seedMap.count(result[i])) {
          terminateState(*result[i]);
          result[i] = NULL;
        }
      } 
    }
  }

  for (unsigned i=0; i<N; ++i)
    if (result[i])
      addConstraint(*result[i], conditions[i]);
}

Executor::StatePair 
Executor::fork(ExecutionState &current, int tid, ref<Expr> condition, bool isInternal) {
#ifdef MEMORY_DEBUG
  size_t before = sys::Process::GetTotalMemoryUsage();
#endif
  Solver::Validity res;
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
    seedMap.find(&current);
  bool isSeeding = it != seedMap.end();
  if (UsePathSlicer)
    pathSlicer->startSymQuery((void *)&current);

  if (!isSeeding && !isa<ConstantExpr>(condition) && 
      (MaxStaticForkPct!=1. || MaxStaticSolvePct != 1. ||
       MaxStaticCPForkPct!=1. || MaxStaticCPSolvePct != 1.) &&
      statsTracker->elapsed() > 60.) {
    StatisticManager &sm = *theStatisticManager;
    CallPathNode *cpn = current.lastThread()->stack.back().callPathNode;
    if ((MaxStaticForkPct<1. &&
         sm.getIndexedValue(stats::forks, sm.getIndex()) > 
         stats::forks*MaxStaticForkPct) ||
        (MaxStaticCPForkPct<1. &&
         cpn && (cpn->statistics.getValue(stats::forks) > 
                 stats::forks*MaxStaticCPForkPct)) ||
        (MaxStaticSolvePct<1 &&
         sm.getIndexedValue(stats::solverTime, sm.getIndex()) > 
         stats::solverTime*MaxStaticSolvePct) ||
        (MaxStaticCPForkPct<1. &&
         cpn && (cpn->statistics.getValue(stats::solverTime) > 
                 stats::solverTime*MaxStaticCPSolvePct))) {
      ref<ConstantExpr> value; 
      bool success = solver->getValue(current, condition, value);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      addConstraint(current, EqExpr::create(value, condition));
      condition = value;
    }      
  }

  double timeout = stpTimeout;
  if (isSeeding)
    timeout *= it->second.size();
  solver->setTimeout(timeout);
  bool success = solver->evaluate(current, condition, res);
  solver->setTimeout(0);
  if (!success) {
    current.stepBack();
    terminateStateEarly(current, "query timed out");
    return StatePair(0, 0);
  }

  if (!isSeeding) {
    if (replayPath && !isInternal) {
      assert(replayPosition<replayPath->size() &&
             "ran out of branches in replay path mode");
      bool branch = (*replayPath)[replayPosition++];
      
      if (res==Solver::True) {
        assert(branch && "hit invalid branch in replay path mode");
      } else if (res==Solver::False) {
        assert(!branch && "hit invalid branch in replay path mode");
      } else {
        // add constraints
        if(branch) {
          res = Solver::True;
          addConstraint(current, condition);
        } else  {
          res = Solver::False;
          addConstraint(current, Expr::createIsZero(condition));
        }
      }
    } else if (res==Solver::Unknown) {
      assert(!replayOut && "in replay mode, only one branch can be true.");
      
      if ((MaxMemoryInhibit && atMemoryLimit) || 
          current.forkDisabled ||
          inhibitForking || 
          (MaxForks!=~0u && stats::forks >= MaxForks)) {

	if (MaxMemoryInhibit && atMemoryLimit)
	  klee_warning_once(0, "skipping fork (memory cap exceeded)");
	else if (current.forkDisabled)
	  klee_warning_once(0, "skipping fork (fork disabled on current path)");
	else if (inhibitForking)
	  klee_warning_once(0, "skipping fork (fork disabled globally)");
	else 
	  klee_warning_once(0, "skipping fork (max-forks reached)");

        TimerStatIncrementer timer(stats::forkTime);
        if (theRNG.getBool()) {
          addConstraint(current, condition);
          res = Solver::True;        
        } else {
          addConstraint(current, Expr::createIsZero(condition));
          res = Solver::False;
        }
      }
    }
  }

  // Fix branch in only-replay-seed mode, if we don't have both true
  // and false seeds.
  if (isSeeding && 
      (current.forkDisabled || OnlyReplaySeeds) && 
      res == Solver::Unknown) {
    bool trueSeed=false, falseSeed=false;
    // Is seed extension still ok here?
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(), 
           siie = it->second.end(); siit != siie; ++siit) {
      ref<ConstantExpr> res;
      bool success = 
        solver->getValue(current, siit->assignment.evaluate(condition), res);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      if (res->isTrue()) {
        trueSeed = true;
      } else {
        falseSeed = true;
      }
      if (trueSeed && falseSeed)
        break;
    }
    if (!(trueSeed && falseSeed)) {
      assert(trueSeed || falseSeed);
      
      res = trueSeed ? Solver::True : Solver::False;
      addConstraint(current, trueSeed ? condition : Expr::createIsZero(condition));
    }
  }

  if (UsePathSlicer)
    pathSlicer->endSymQuery((void *)&current, (int)res);

  // XXX - even if the constraint is provable one way or the other we
  // can probably benefit by adding this constraint and allowing it to
  // reduce the other constraints. For example, if we do a binary
  // search on a particular value, and then see a comparison against
  // the value it has been fixed at, we should take this as a nice
  // hint to just use the single constraint instead of all the binary
  // search ones. If that makes sense.
  if (res==Solver::True) {
    if (!isInternal) {
      if (pathWriter) {
        current.pathOS << "1";
      }
    }

    return StatePair(&current, 0);
  } else if (res==Solver::False) {
    if (!isInternal) {
      if (pathWriter) {
        current.pathOS << "0";
      }
    }

    return StatePair(0, &current);
  } else {  
    TimerStatIncrementer timer(stats::forkTime);
    ExecutionState *falseState, *trueState = &current;

    ++stats::forks;

    falseState = trueState->branch();
    addedStates.insert(falseState);
    if (UsePathSlicer) {
      pathSlicer->copyTrace((void *)falseState, (void *)trueState);
      size_t dynInstrIdx = pathSlicer->getLatestBrOrExtCallIdx((void *)trueState);
      brTree.addBranch(trueState, dynInstrIdx, falseState);
    }

    if (RandomizeFork && theRNG.getBool())
      std::swap(trueState, falseState);

    if (it != seedMap.end()) {
      std::vector<SeedInfo> seeds = it->second;
      it->second.clear();
      std::vector<SeedInfo> &trueSeeds = seedMap[trueState];
      std::vector<SeedInfo> &falseSeeds = seedMap[falseState];
      for (std::vector<SeedInfo>::iterator siit = seeds.begin(), 
             siie = seeds.end(); siit != siie; ++siit) {
        ref<ConstantExpr> res;
        bool success = 
          solver->getValue(current, siit->assignment.evaluate(condition), res);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;
        if (res->isTrue()) {
          trueSeeds.push_back(*siit);
        } else {
          falseSeeds.push_back(*siit);
        }
      }
      
      bool swapInfo = false;
      if (trueSeeds.empty()) {
        if (&current == trueState) swapInfo = true;
        seedMap.erase(trueState);
      }
      if (falseSeeds.empty()) {
        if (&current == falseState) swapInfo = true;
        seedMap.erase(falseState);
      }
      if (swapInfo) {
        std::swap(trueState->coveredNew, falseState->coveredNew);
        std::swap(trueState->coveredLines, falseState->coveredLines);
      }
    }

    current.ptreeNode->data = 0;
    std::pair<PTree::Node*, PTree::Node*> res =
      processTree->split(current.ptreeNode, falseState, trueState);
    falseState->ptreeNode = res.first;
    trueState->ptreeNode = res.second;

    if (!isInternal) {
      if (pathWriter) {
        falseState->pathOS = pathWriter->open(current.pathOS);
        trueState->pathOS << "1";
        falseState->pathOS << "0";
      }      
      if (symPathWriter) {
        falseState->symPathOS = symPathWriter->open(current.symPathOS);
        trueState->symPathOS << "1";
        falseState->symPathOS << "0";
      }
    }

    addConstraint(*trueState, condition);
    addConstraint(*falseState, Expr::createIsZero(condition));

    // Kinda gross, do we even really still want this option?
    if (MaxDepth && MaxDepth<=trueState->depth) {
      terminateStateEarly(*trueState, "max-depth exceeded");
      terminateStateEarly(*falseState, "max-depth exceeded");
      return StatePair(0, 0);
    }

#ifdef MEMORY_DEBUG
    size_t end = sys::Process::GetTotalMemoryUsage();
    std::cerr << "mem usage fork(): " << std::dec  << before << "/" << end << " diff: " << (end-before)<< "\n";
#endif
    return StatePair(trueState, falseState);
  }
}

void Executor::addConstraint(ExecutionState &state, ref<Expr> condition) {
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(condition)) {
    assert(CE->isTrue() && "attempt to add invalid constraint");
    return;
  }

  // Check to see if this constraint violates seeds.
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
    seedMap.find(&state);
  if (it != seedMap.end()) {
    bool warn = false;
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(), 
           siie = it->second.end(); siit != siie; ++siit) {
      bool res;
      bool success = 
        solver->mustBeFalse(state, siit->assignment.evaluate(condition), res);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      if (res) {
        siit->patchSeed(state, condition, solver);
        warn = true;
      }
    }
    if (warn)
      klee_warning("seeds patched for violating constraint"); 
  }

  state.addConstraint(condition);
  if (ivcEnabled)
    doImpliedValueConcretization(state, condition, 
                                 ConstantExpr::alloc(1, Expr::Bool));
}

ref<klee::ConstantExpr> Executor::evalConstant(const Constant *c) {
  if (const llvm::ConstantExpr *ce = dyn_cast<llvm::ConstantExpr>(c)) {
    return evalConstantExpr(ce);
  } else {
    if (const ConstantInt *ci = dyn_cast<ConstantInt>(c)) {
      return ConstantExpr::alloc(ci->getValue());
    } else if (const ConstantFP *cf = dyn_cast<ConstantFP>(c)) {      
      return ConstantExpr::alloc(cf->getValueAPF().bitcastToAPInt());
    } else if (const GlobalValue *gv = dyn_cast<GlobalValue>(c)) {
      return globalAddresses.find(gv)->second;
    } else if (isa<ConstantPointerNull>(c)) {
      return Expr::createPointer(0);
    } else if (isa<UndefValue>(c) || isa<ConstantAggregateZero>(c)) {
      return ConstantExpr::create(0, getWidthForLLVMType(c->getType()));
    } else if (const ConstantStruct *cs = dyn_cast<ConstantStruct>(c)) {
      const StructLayout *sl = kmodule->targetData->getStructLayout(cs->getType());
      llvm::SmallVector<ref<Expr>, 4> kids;
      for (unsigned i = cs->getNumOperands(); i != 0; --i) {
        unsigned op = i-1;
        ref<Expr> kid = evalConstant(cs->getOperand(op));

        uint64_t thisOffset = sl->getElementOffsetInBits(op),
                 nextOffset = (op == cs->getNumOperands() - 1)
                              ? sl->getSizeInBits()
                              : sl->getElementOffsetInBits(op+1);
        if (nextOffset-thisOffset > kid->getWidth()) {
          uint64_t paddingWidth = nextOffset-thisOffset-kid->getWidth();
          kids.push_back(ConstantExpr::create(0, paddingWidth));
        }

        kids.push_back(kid);
      }
      ref<Expr> res = ConcatExpr::createN(kids.size(), kids.data());
      return cast<ConstantExpr>(res);
    } else {
      // Constant{Array,Vector}
      assert(0 && "invalid argument to evalConstant()");
    }
  }
}

const Cell& Executor::eval(KInstruction *ki, unsigned index, 
                           ThreadState &state) const {
  assert(index < ki->inst->getNumOperands());
  int vnumber = ki->operands[index];

  assert(vnumber != -1 &&
         "Invalid operand to eval(), not a value or constant!");

  // Determine if this is a constant or not.
  if (vnumber < 0) {
    unsigned index = -vnumber - 2;
    return kmodule->constantTable[index];
  } else {
    unsigned index = vnumber;
    StackFrame &sf = state.stack.back();
    return sf.locals[index];
  }
}

void Executor::bindLocal(KInstruction *target, ThreadState &state, 
                         ref<Expr> value) {
  getDestCell(state, target).value = value;
}

void Executor::bindArgument(KFunction *kf, unsigned index, 
                            ThreadState &state, ref<Expr> value) {
  getArgumentCell(state, kf, index).value = value;
}

ref<Expr> Executor::toUnique(const ExecutionState &state, 
                             ref<Expr> &e) {
  ref<Expr> result = e;

  if (!isa<ConstantExpr>(e)) {
    ref<ConstantExpr> value;
    bool isTrue = false;

    solver->setTimeout(stpTimeout);      
    if (solver->getValue(state, e, value) &&
        solver->mustBeTrue(state, EqExpr::create(e, value), isTrue) &&
        isTrue)
      result = value;
    solver->setTimeout(0);
  }
  
  return result;
}


/* Concretize the given expression, and return a possible constant value. 
   'reason' is just a documentation string stating the reason for concretization. */
ref<klee::ConstantExpr> 
Executor::toConstant(ExecutionState &state, 
                     ref<Expr> e,
                     const char *reason) {
  e = state.constraints.simplifyExpr(e);
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(e))
    return CE;

  ref<ConstantExpr> value;
  bool success = solver->getValue(state, e, value);
  assert(success && "FIXME: Unhandled solver failure");
  (void) success;
    
  std::ostringstream os;
  os << "silently concretizing (reason: " << reason << ") expression " << e 
     << " to value " << value 
     << " (" << (*(state.lastThread()->pc)).info->file << ":" << (*(state.lastThread()->pc)).info->line << ")";
      
  if (AllExternalWarnings)
    klee_warning(reason, os.str().c_str());
  else
    klee_warning_once(reason, "%s", os.str().c_str());

  addConstraint(state, EqExpr::create(e, value));
    
  return value;
}

void Executor::executeGetValue(ExecutionState &state,
                               ThreadState &thr,
                               ref<Expr> e,
                               KInstruction *target) {
  e = state.constraints.simplifyExpr(e);
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
    seedMap.find(&state);
  if (it==seedMap.end() || isa<ConstantExpr>(e)) {
    ref<ConstantExpr> value;
    bool success = solver->getValue(state, e, value);
    assert(success && "FIXME: Unhandled solver failure");
    (void) success;
    bindLocal(target, thr, value);
  } else {
    std::set< ref<Expr> > values;
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(), 
           siie = it->second.end(); siit != siie; ++siit) {
      ref<ConstantExpr> value;
      bool success = 
        solver->getValue(state, siit->assignment.evaluate(e), value);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      values.insert(value);
    }
    
    std::vector< ref<Expr> > conditions;
    for (std::set< ref<Expr> >::iterator vit = values.begin(), 
           vie = values.end(); vit != vie; ++vit)
      conditions.push_back(EqExpr::create(e, *vit));

    std::vector<ExecutionState*> branches;
    branch(state, conditions, branches);
    
    std::vector<ExecutionState*>::iterator bit = branches.begin();
    for (std::set< ref<Expr> >::iterator vit = values.begin(), 
           vie = values.end(); vit != vie; ++vit) {
      ExecutionState *es = *bit;
      if (es)
        bindLocal(target, *es->getThread(thr.id), *vit);
      ++bit;
    }
  }
}

void Executor::stepInstruction(ExecutionState &state, int tid) {
  if (DebugPrintInstructions) {
    printFileLine(state, state.getThread(tid)->pc);
    std::cerr << std::setw(10) << stats::instructions << " ";
    llvm::errs() << *(state.getThread(tid)->pc->inst);
  }

  if (statsTracker)
    statsTracker->stepInstruction(state, tid);

  ++stats::instructions;
  if (UsePathSlicer)
    pathSlicer->collectNotPrunedInstrs((void *)&state);
  state.getThread(tid)->prevPC = state.getThread(tid)->pc;
  ++state.getThread(tid)->pc;

  if (stats::instructions==StopAfterNInstructions)
    haltExecution = true;
}

void Executor::executeCall(ExecutionState &state, 
                           ThreadState &thr,
                           KInstruction *ki,
                           Function *f,
                           std::vector< ref<Expr> > &arguments) {
  Instruction *i = ki->inst;
  if (f && f->isDeclaration()) {
    switch(f->getIntrinsicID()) {
    case Intrinsic::not_intrinsic:
      // state may be destroyed by this call, cannot touch
      /* Heming: now the called function f is resolved, we can record this external call. */
      if (UsePathSlicer)
        pathSlicer->record((void *)&state, (void *)ki, (void *)&thr, (void *)f);
      callExternalFunction(state, thr, ki, f, arguments);
      break;
        
      // va_arg is handled by caller and intrinsic lowering, see comment for
      // ExecutionState::varargs
    case Intrinsic::vastart:  {
      StackFrame &sf = thr.stack.back();
      assert(sf.varargs && 
             "vastart called in function with no vararg object");

      // FIXME: This is really specific to the architecture, not the pointer
      // size. This happens to work fir x86-32 and x86-64, however.
      Expr::Width WordSize = Context::get().getPointerWidth();
      if (WordSize == Expr::Int32) {
        executeMemoryOperation(state, thr, true, arguments[0], 
                               sf.varargs->getBaseExpr(), 0);
      } else {
        assert(WordSize == Expr::Int64 && "Unknown word size!");

        // X86-64 has quite complicated calling convention. However,
        // instead of implementing it, we can do a simple hack: just
        // make a function believe that all varargs are on stack.
        executeMemoryOperation(state, thr, true, arguments[0],
                               ConstantExpr::create(48, 32), 0); // gp_offset
        executeMemoryOperation(state, thr, true,
                               AddExpr::create(arguments[0], 
                                               ConstantExpr::create(4, 64)),
                               ConstantExpr::create(304, 32), 0); // fp_offset
        executeMemoryOperation(state, thr, true,
                               AddExpr::create(arguments[0], 
                                               ConstantExpr::create(8, 64)),
                               sf.varargs->getBaseExpr(), 0); // overflow_arg_area
        executeMemoryOperation(state, thr, true,
                               AddExpr::create(arguments[0], 
                                               ConstantExpr::create(16, 64)),
                               ConstantExpr::create(0, 64), 0); // reg_save_area
      }
      break;
    }
    case Intrinsic::vaend:
      // va_end is a noop for the interpreter.
      //
      // FIXME: We should validate that the target didn't do something bad
      // with vaeend, however (like call it twice).
      break;
        
    case Intrinsic::vacopy:
      // va_copy should have been lowered.
      //
      // FIXME: It would be nice to check for errors in the usage of this as
      // well.
    default:
      klee_error("unknown intrinsic: %s", f->getName().data());
    }

    if (InvokeInst *ii = dyn_cast<InvokeInst>(i))
      transferToBasicBlock(ii->getNormalDest(), i->getParent(), thr);
  } else {
    // FIXME: I'm not really happy about this reliance on prevPC but it is ok, I
    // guess. This just done to avoid having to pass KInstIterator everywhere
    // instead of the actual instruction, since we can't make a KInstIterator
    // from just an instruction (unlike LLVM).
    KFunction *kf = kmodule->functionMap[f];
    thr.pushFrame(thr.prevPC, kf);
    thr.pc = kf->instructions;
    
    /* Heming: now the called function f is resolved, we can record this internal call. */
    if (UsePathSlicer)
      pathSlicer->record((void *)&state, (void *)ki, (void *)&thr, (void *)f);
        
    if (statsTracker)
      statsTracker->framePushed(state, &thr.stack[thr.stack.size()-2], thr.id);

     // TODO: support "byval" parameter attribute
     // TODO: support zeroext, signext, sret attributes
        
    unsigned callingArgs = arguments.size();
    unsigned funcArgs = f->arg_size();
    if (!f->isVarArg()) {
      if (callingArgs > funcArgs) {
        klee_warning_once(f, "calling %s with extra arguments.", 
                          f->getName().data());
      } else if (callingArgs < funcArgs) {
        terminateStateOnError(state, "calling function with too few arguments", 
                              "user.err");
        return;
      }
    } else {
      if (callingArgs < funcArgs) {
        terminateStateOnError(state, "calling function with too few arguments", 
                              "user.err");
        return;
      }
            
      StackFrame &sf = thr.stack.back();
      unsigned size = 0;
      for (unsigned i = funcArgs; i < callingArgs; i++) {
        // FIXME: This is really specific to the architecture, not the pointer
        // size. This happens to work fir x86-32 and x86-64, however.
        Expr::Width WordSize = Context::get().getPointerWidth();
        if (WordSize == Expr::Int32) {
          size += Expr::getMinBytesForWidth(arguments[i]->getWidth());
        } else {
          size += llvm::RoundUpToAlignment(arguments[i]->getWidth(), 
                                           WordSize) / 8;
        }
      }

      MemoryObject *mo = sf.varargs = memory->allocate(size, true, false, 
                                                       thr.prevPC->inst);
      if (!mo) {
        terminateStateOnExecError(state, "out of memory (varargs)");
        return;
      }
      ObjectState *os = bindObjectInState(state, &thr, mo, true);
      unsigned offset = 0;
      for (unsigned i = funcArgs; i < callingArgs; i++) {
        // FIXME: This is really specific to the architecture, not the pointer
        // size. This happens to work fir x86-32 and x86-64, however.
        Expr::Width WordSize = Context::get().getPointerWidth();
        if (WordSize == Expr::Int32) {
          os->write(offset, arguments[i]);
          offset += Expr::getMinBytesForWidth(arguments[i]->getWidth());
        } else {
          assert(WordSize == Expr::Int64 && "Unknown word size!");
          os->write(offset, arguments[i]);
          offset += llvm::RoundUpToAlignment(arguments[i]->getWidth(), 
                                             WordSize) / 8;
        }
      }
    }

    unsigned numFormals = f->arg_size();
    for (unsigned i=0; i<numFormals; ++i) 
      bindArgument(kf, i, thr, arguments[i]);

    if (!UsePathSlicer || curInstrIsInternal(ki)) {
      Checker::Result globalResult = globalCheckers.onCall(state, thr, ki, f, arguments, NULL);
      Checker::Result localResult = state.localCheckers.onCall(state, thr, ki, f, arguments, NULL);
      if (UsePathSlicer)
        pathSlicer->recordCheckerResult((void *)&state, globalResult, 
          localResult, interpreterHandler->getNumTestCases());
      if (globalResult & Checker::ERROR || localResult & Checker::ERROR)
      {
          terminateStateOnError(state, "checker reported error", "check.err");
          return;
      }
    }
  }
}

void Executor::transferToBasicBlock(BasicBlock *dst, BasicBlock *src, 
                                    ThreadState &state) {
  // Note that in general phi nodes can reuse phi values from the same
  // block but the incoming value is the eval() result *before* the
  // execution of any phi nodes. this is pathological and doesn't
  // really seem to occur, but just in case we run the PhiCleanerPass
  // which makes sure this cannot happen and so it is safe to just
  // eval things in order. The PhiCleanerPass also makes sure that all
  // incoming blocks have the same order for each PHINode so we only
  // have to compute the index once.
  //
  // With that done we simply set an index in the state so that PHI
  // instructions know which argument to eval, set the pc, and continue.
  
  // XXX this lookup has to go ?
  KFunction *kf = state.stack.back().kf;
  unsigned entry = kf->basicBlockEntry[dst];
  state.pc = &kf->instructions[entry];
  if (state.pc->inst->getOpcode() == Instruction::PHI) {
    PHINode *first = static_cast<PHINode*>(state.pc->inst);
    state.incomingBBIndex = first->getBasicBlockIndex(src);
  }
}

void Executor::printFileLine(ExecutionState &state, KInstruction *ki) {
  const InstructionInfo &ii = *ki->info;
  if (ii.file != "") 
    std::cerr << "     " << ii.file << ":" << ii.line << ":";
  else
    std::cerr << "     [no debug info]:";
}

/// Compute the true target of a function call, resolving LLVM and KLEE aliases
/// and bitcasts.
Function* Executor::getTargetFunction(Value *calledVal, ExecutionState &state) {
  SmallPtrSet<const GlobalValue*, 3> Visited;

  Constant *c = dyn_cast<Constant>(calledVal);
  if (!c)
    return 0;

  while (true) {
    if (GlobalValue *gv = dyn_cast<GlobalValue>(c)) {
      if (!Visited.insert(gv))
        return 0;

      std::string alias = state.getFnAlias(gv->getName());
      if (alias != "") {
        llvm::Module* currModule = kmodule->module;
        GlobalValue *old_gv = gv;
        gv = currModule->getNamedValue(alias);
        if (!gv) {
          llvm::errs() << "Function " << alias << "(), alias for " 
                       << old_gv->getName() << " not found!\n";
          assert(0 && "function alias not found");
        }
      }
     
      if (Function *f = dyn_cast<Function>(gv))
        return f;
      else if (GlobalAlias *ga = dyn_cast<GlobalAlias>(gv))
        c = ga->getAliasee();
      else
        return 0;
    } else if (llvm::ConstantExpr *ce = dyn_cast<llvm::ConstantExpr>(c)) {
      if (ce->getOpcode()==Instruction::BitCast)
        c = ce->getOperand(0);
      else
        return 0;
    } else
      return 0;
  }
}

static bool isDebugIntrinsic(const Function *f, KModule *KM) {
#if LLVM_VERSION_CODE < LLVM_VERSION(2, 7)
  // Fast path, getIntrinsicID is slow.
  if (f == KM->dbgStopPointFn)
    return true;

  switch (f->getIntrinsicID()) {
  case Intrinsic::dbg_stoppoint:
  case Intrinsic::dbg_region_start:
  case Intrinsic::dbg_region_end:
  case Intrinsic::dbg_func_start:
  case Intrinsic::dbg_declare:
    return true;

  default:
    return false;
  }
#else
  return false;
#endif
}

static inline const llvm::fltSemantics * fpWidthToSemantics(unsigned width) {
  switch(width) {
  case Expr::Int32:
    return &llvm::APFloat::IEEEsingle;
  case Expr::Int64:
    return &llvm::APFloat::IEEEdouble;
  case Expr::Fl80:
    return &llvm::APFloat::x87DoubleExtended;
  default:
    return 0;
  }
}

bool Executor::willBlock(ExecutionState &state, int tid, std::set<int>& enable_set)
{
    KInstruction *ki = state.getThread(tid)->pc;
    ThreadState &thr = *state.getThread(tid);

    if (ki->inst->getOpcode() == Instruction::Call)
    {
        Instruction *i = ki->inst;
        CallInst *ci = cast<CallInst>(ki->inst);
        Value *func = ci->getCalledValue();

        // BEGIN: copy from executeInstruction

        CallSite cs(i);

        unsigned numArgs = cs.arg_size();
        Value *fp = cs.getCalledValue();
        Function *f = getTargetFunction(fp, state);

        // Skip debug intrinsics, we can't evaluate their metadata arguments.
        if (f && isDebugIntrinsic(f, kmodule))
            return false;

        // evaluate arguments
        std::vector< ref<Expr> > arguments;
        arguments.reserve(numArgs);

        for (unsigned j=0; j<numArgs; ++j)
            arguments.push_back(eval(ki, j+1, thr).value);

        if (f) {
            const FunctionType *fType = 
                dyn_cast<FunctionType>(cast<PointerType>(f->getType())->getElementType());
            const FunctionType *fpType =
                dyn_cast<FunctionType>(cast<PointerType>(fp->getType())->getElementType());

            // special case the call with a bitcast case
            if (fType != fpType) {
                assert(fType && fpType && "unable to get function type");

                // XXX check result coercion

                // XXX this really needs thought and validation
                unsigned i=0;
                for (std::vector< ref<Expr> >::iterator
                        ai = arguments.begin(), ie = arguments.end();
                        ai != ie; ++ai) {
                    Expr::Width to, from = (*ai)->getWidth();

                    if (i<fType->getNumParams()) {
                        to = getWidthForLLVMType(fType->getParamType(i));

                        if (from != to) {
                            // XXX need to check other param attrs ?
                            if (cs.paramHasAttr(i+1, llvm::Attribute::SExt)) {
                                arguments[i] = SExtExpr::create(arguments[i], to);
                            } else {
                                arguments[i] = ZExtExpr::create(arguments[i], to);
                            }
                        }
                    }

                    i++;
                }
            } else if (isa<InlineAsm>(fp)) {
                return false;
            }
        } else {
            assert(false && "not supported: calling pthread with function pointer");
        }

        // END: copy from executeInstruction()

        // BEGIN: copy from callExternalFunction()

        // normal external function handling path
        // allocate 128 bits for each argument (+return value) to support fp80's;
        // we could iterate through all the arguments first and determine the exact
        // size we need, but this is faster, and the memory usage isn't significant.
        uint64_t *args = (uint64_t*) alloca(2*sizeof(*args) * (arguments.size() + 1));
        memset(args, 0, 2 * sizeof(*args) * (arguments.size() + 1));
        unsigned wordIndex = 2;
        for (std::vector<ref<Expr> >::iterator ai = arguments.begin(), 
                ae = arguments.end(); ai!=ae; ++ai) {
            if (AllowExternalSymCalls) { // don't bother checking uniqueness
                ref<ConstantExpr> ce;
                bool success = solver->getValue(state, *ai, ce);
                assert(success && "FIXME: Unhandled solver failure");
                (void) success;
                ce->toMemory(&args[wordIndex]);
                wordIndex += (ce->getWidth()+63)/64;
            } else {
                ref<Expr> arg = toUnique(state, *ai);
                if (ConstantExpr *ce = dyn_cast<ConstantExpr>(arg)) {
                    // XXX kick toMemory functions from here
                    ce->toMemory(&args[wordIndex]);
                    wordIndex += (ce->getWidth()+63)/64;
                } else {
                    return false;
                }
            }
        }

        state.addressSpace.copyOutConcretes();

        std::string name = func->getName();
        if (name == "pthread_mutex_init")
        {
            return false;
        } else if (name == "pthread_mutex_lock")
        {
            std::cerr << "MUTEX lock! " << std::endl;
            std::cerr << "mutex: " << args[2] << std::endl;
            uint64_t mutex = args[2];
            if (state.mutexes.find(mutex) != state.mutexes.end())
            {
                MutexInfo &m_info = state.mutexes[mutex];
                if (m_info.acquired)
                {
                    m_info.waiters.push(tid);
                    return true;
                }
                else
                    return false;
            }
            else
            {
                std::cerr << "Warning: uninited mutex!" << std::endl;
                return false;
            }
        } else if (name == "pthread_mutex_unlock")
        {
            std::cerr << "MUTEX unlock!" << std::endl;
            std::cerr << "mutex: " << args[2] << std::endl;
            return false;
        }
    }

    return false;

}

void Executor::executeInstruction(ExecutionState &state, int tid, KInstruction *ki) {
  assert(state.threads.find(tid) != state.threads.end());

  ThreadState &thr = (*state.threads.find(tid)).second;
  Instruction *i = ki->inst;
  /* Heming: for all instructions except Call, we can do recording before it 
  is interpreted. For Call, we have to wait until its function pointer is 
  resolved by KLEE. */
  if (UsePathSlicer && i->getOpcode() != Instruction::Call)
    pathSlicer->record((void *)&state, (void *)ki, (void *)&thr, NULL);
  switch (i->getOpcode()) {
    // Control flow
  case Instruction::Ret: {
    ReturnInst *ri = cast<ReturnInst>(i);
    KInstIterator kcaller = thr.stack.back().caller;
    Instruction *caller = kcaller ? kcaller->inst : 0;
    bool isVoidReturn = (ri->getNumOperands() == 0);
    ref<Expr> result = ConstantExpr::alloc(0, Expr::Bool);
    
    if (!isVoidReturn) {
      result = eval(ki, 0, thr).value;
    }
    
    if (thr.stack.size() <= 1) {
      assert(!caller && "caller set on initial stack frame");
      terminateStateOnExit(state);
    } else {
      thr.popFrame();

      if (statsTracker)
        statsTracker->framePopped(state);

      if (InvokeInst *ii = dyn_cast<InvokeInst>(caller)) {
        transferToBasicBlock(ii->getNormalDest(), caller->getParent(), thr);
      } else {
        thr.pc = kcaller;
        ++thr.pc;
      }

      if (!isVoidReturn) {
        LLVM_TYPE_Q Type *t = caller->getType();
        if (t != Type::getVoidTy(getGlobalContext())) {
          // may need to do coercion due to bitcasts
          Expr::Width from = result->getWidth();
          Expr::Width to = getWidthForLLVMType(t);
            
          if (from != to) {
            CallSite cs = (isa<InvokeInst>(caller) ? CallSite(cast<InvokeInst>(caller)) : 
                           CallSite(cast<CallInst>(caller)));

            // XXX need to check other param attrs ?
            if (cs.paramHasAttr(0, llvm::Attribute::SExt)) {
              result = SExtExpr::create(result, to);
            } else {
              result = ZExtExpr::create(result, to);
            }
          }

          bindLocal(kcaller, thr, result);
        }
      } else {
        // We check that the return value has no users instead of
        // checking the type, since C defaults to returning int for
        // undeclared functions.
        if (!caller->use_empty()) {
          terminateStateOnExecError(state, "return void when caller expected a result");
        }
      }

      if (!UsePathSlicer || curInstrIsInternal(&(*kcaller))) {
        Checker::Result globalResult = globalCheckers.onRet(state, thr, &(*kcaller), result);
        Checker::Result localResult = state.localCheckers.onRet(state, thr, &(*kcaller), result);
        if (UsePathSlicer)
          pathSlicer->recordCheckerResult((void *)&state, globalResult,
            localResult, interpreterHandler->getNumTestCases());
        if (globalResult & Checker::ERROR || localResult & Checker::ERROR)
        {
            terminateStateOnError(state, "checker reported error", "check.err");
            return;
        }
      }

    }      
    break;
  }
  case Instruction::Unwind: {
    for (;;) {
      KInstruction *kcaller = thr.stack.back().caller;
      thr.popFrame();

      if (statsTracker)
        statsTracker->framePopped(state);

      if (thr.stack.empty()) {
        terminateStateOnExecError(state, "unwind from initial stack frame");
        break;
      } else {
        Instruction *caller = kcaller->inst;
        if (InvokeInst *ii = dyn_cast<InvokeInst>(caller)) {
          transferToBasicBlock(ii->getUnwindDest(), caller->getParent(), thr);
          break;
        }
      }
    }
    break;
  }
  case Instruction::Br: {
    BranchInst *bi = cast<BranchInst>(i);
    if (bi->isUnconditional()) {
      transferToBasicBlock(bi->getSuccessor(0), bi->getParent(), thr);
    } else {
      // FIXME: Find a way that we don't have this hidden dependency.
      assert(bi->getCondition() == bi->getOperand(0) &&
             "Wrong operand index!");
      ref<Expr> cond = eval(ki, 0, thr).value;
      Executor::StatePair branches = fork(state, tid, cond, false);

      // NOTE: There is a hidden dependency here, markBranchVisited
      // requires that we still be in the context of the branch
      // instruction (it reuses its statistic id). Should be cleaned
      // up with convenient instruction specific data.
      if (statsTracker && thr.stack.back().kf->trackCoverage)
        statsTracker->markBranchVisited(branches.first, branches.second);

      if (!UsePathSlicer || curInstrIsInternal(ki)) {
        Checker::Result globalResult = 
          globalCheckers.onBranch(state, thr, bi, ki, cond, branches.first, branches.second);
        Checker::Result localResult = 
          state.localCheckers.onBranch(state, thr, bi, ki, cond, branches.first, branches.second);
        if (UsePathSlicer)
          pathSlicer->recordCheckerResult((void *)&state, globalResult,
            localResult, interpreterHandler->getNumTestCases());
      }

      if (branches.first)
        transferToBasicBlock(bi->getSuccessor(0), bi->getParent(), *branches.first->getThread(tid));
      if (branches.second)
        transferToBasicBlock(bi->getSuccessor(1), bi->getParent(), *branches.second->getThread(tid));
    }
    break;
  }
  case Instruction::Switch: {
    SwitchInst *si = cast<SwitchInst>(i);
    ref<Expr> cond = eval(ki, 0, thr).value;
    unsigned cases = si->getNumCases();
    BasicBlock *bb = si->getParent();

    cond = toUnique(state, cond);
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(cond)) {
      // Somewhat gross to create these all the time, but fine till we
      // switch to an internal rep.
      LLVM_TYPE_Q llvm::IntegerType *Ty = 
        cast<IntegerType>(si->getCondition()->getType());
      ConstantInt *ci = ConstantInt::get(Ty, CE->getZExtValue());
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 1)
      unsigned index = si->resolveSuccessorIndex(si->findCaseValue(ci));
#else
      unsigned index = si->findCaseValue(ci);
#endif
      transferToBasicBlock(si->getSuccessor(index), si->getParent(), thr);
    } else {
      std::map<BasicBlock*, ref<Expr> > targets;
      ref<Expr> isDefault = ConstantExpr::alloc(1, Expr::Bool);
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 1)      
      for (unsigned i=0; i<cases; ++i) {
#else
      for (unsigned i=1; i<cases; ++i) {  
#endif
        ref<Expr> value = evalConstant(si->getCaseValue(i));
        ref<Expr> match = EqExpr::create(cond, value);
        isDefault = AndExpr::create(isDefault, Expr::createIsZero(match));
        bool result;
        bool success = solver->mayBeTrue(state, match, result);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;
        if (result) {
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 1)
          BasicBlock *caseSuccessor = si->getCaseSuccessor(i);
#else
          BasicBlock *caseSuccessor = si->getSuccessor(i);
#endif
          std::map<BasicBlock*, ref<Expr> >::iterator it =
            targets.insert(std::make_pair(caseSuccessor,
                           ConstantExpr::alloc(0, Expr::Bool))).first;

          it->second = OrExpr::create(match, it->second);
        }
      }
      bool res;
      bool success = solver->mayBeTrue(state, isDefault, res);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      if (res)
        targets.insert(std::make_pair(si->getDefaultDest(), isDefault));
      
      std::vector< ref<Expr> > conditions;
      for (std::map<BasicBlock*, ref<Expr> >::iterator it = 
             targets.begin(), ie = targets.end();
           it != ie; ++it)
        conditions.push_back(it->second);
      
      std::vector<ExecutionState*> branches;
      if (UsePathSlicer) { // Heming: switch are in external functions only, this does not matter in branch tree, let it go.
        if (curInstrIsInternal(ki)) {
          std::cerr << "Hitting internal switch statements..." << endl;
          abort();//Heming: if use path slicer, switches should have already been lowered to branches.
        }
      }
      branch(state, conditions, branches);
        
      std::vector<ExecutionState*>::iterator bit = branches.begin();
      for (std::map<BasicBlock*, ref<Expr> >::iterator it = 
             targets.begin(), ie = targets.end();
           it != ie; ++it) {
        ExecutionState *es = *bit;
        if (es)
          transferToBasicBlock(it->first, bb, *es->getThread(tid));
        ++bit;
      }
    }
    break;
 }
  case Instruction::Unreachable:
    // Note that this is not necessarily an internal bug, llvm will
    // generate unreachable instructions in cases where it knows the
    // program will crash. So it is effectively a SEGV or internal
    // error.
    terminateStateOnExecError(state, "reached \"unreachable\" instruction");
    break;

  case Instruction::Invoke:
  case Instruction::Call: {
    CallSite cs(i);

    unsigned numArgs = cs.arg_size();
    Value *fp = cs.getCalledValue();
    Function *f = getTargetFunction(fp, state);

    if (f)
    {
        std::string name = f->getName();
//        std::cout << "calling " << name << " value: " << fp << std::endl;
    } else {
//        std::cout << "calling (unknown) value: " << fp << std::endl;
//        errs() << *fp << "\n";
        InlineAsm *ia = dyn_cast<InlineAsm>(fp);
        if (ia)
        {
            errs() << "Inline asm!\n";
            terminateStateOnExecError(state, "inline asm!");
            break;
        }
    }

    // Skip debug intrinsics, we can't evaluate their metadata arguments.
    if (f && isDebugIntrinsic(f, kmodule))
      break;

    // evaluate arguments
    std::vector< ref<Expr> > arguments;
    arguments.reserve(numArgs);

    for (unsigned j=0; j<numArgs; ++j)
      arguments.push_back(eval(ki, j+1, thr).value);

    if (f) {
      const FunctionType *fType = 
        dyn_cast<FunctionType>(cast<PointerType>(f->getType())->getElementType());
      const FunctionType *fpType =
        dyn_cast<FunctionType>(cast<PointerType>(fp->getType())->getElementType());

      // special case the call with a bitcast case
      if (fType != fpType) {
        assert(fType && fpType && "unable to get function type");

        // XXX check result coercion

        // XXX this really needs thought and validation
        unsigned i=0;
        for (std::vector< ref<Expr> >::iterator
               ai = arguments.begin(), ie = arguments.end();
             ai != ie; ++ai) {
          Expr::Width to, from = (*ai)->getWidth();
            
          if (i<fType->getNumParams()) {
            to = getWidthForLLVMType(fType->getParamType(i));

            if (from != to) {
              // XXX need to check other param attrs ?
              if (cs.paramHasAttr(i+1, llvm::Attribute::SExt)) {
                arguments[i] = SExtExpr::create(arguments[i], to);
              } else {
                arguments[i] = ZExtExpr::create(arguments[i], to);
              }
            }
          }
            
          i++;
        }
      } else if (isa<InlineAsm>(fp)) {
        terminateStateOnExecError(state, "inline assembly is unsupported");
        break;
      }

      state.curFunc = f;
      state.curArguments = &arguments;
      executeCall(state, thr, ki, f, arguments);
    } else {
      ref<Expr> v = eval(ki, 0, thr).value;

      ExecutionState *free = &state;
      bool hasInvalid = false, first = true;

      /* XXX This is wasteful, no need to do a full evaluate since we
         have already got a value. But in the end the caches should
         handle it for us, albeit with some overhead. */
      do {
        ref<ConstantExpr> value;
        bool success = solver->getValue(*free, v, value);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;
        StatePair res = fork(*free, tid, EqExpr::create(v, value), true);
        if (res.first) {
          uint64_t addr = value->getZExtValue();
          if (legalFunctions.count(addr)) {
            f = (Function*) addr;

            // Don't give warning on unique resolution
            if (res.second || !first)
              klee_warning_once((void*) (unsigned long) addr, 
                                "resolved symbolic function pointer to: %s",
                                f->getName().data());

            state.curFunc = f;
            state.curArguments = &arguments;
            executeCall(*res.first, *res.first->getThread(tid), ki, f, arguments);
          } else {
            if (!hasInvalid) {
              terminateStateOnExecError(state, "invalid function pointer");
              hasInvalid = true;
            }
          }
        }

        first = false;
        free = res.second;
      } while (free);
    }
    break;
  }
  case Instruction::PHI: {
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 0)
    ref<Expr> result = eval(ki, thr.incomingBBIndex, thr).value;
#else
    ref<Expr> result = eval(ki, thr.incomingBBIndex * 2, thr).value;
#endif
    bindLocal(ki, thr, result);
    break;
  }

    // Special instructions
  case Instruction::Select: {
    SelectInst *SI = cast<SelectInst>(ki->inst);
    assert(SI->getCondition() == SI->getOperand(0) &&
           "Wrong operand index!");
    ref<Expr> cond = eval(ki, 0, thr).value;
    ref<Expr> tExpr = eval(ki, 1, thr).value;
    ref<Expr> fExpr = eval(ki, 2, thr).value;
    ref<Expr> result = SelectExpr::create(cond, tExpr, fExpr);
    bindLocal(ki, thr, result);
    break;
  }

  case Instruction::VAArg:
    terminateStateOnExecError(state, "unexpected VAArg instruction");
    break;

    // Arithmetic / logical

  case Instruction::Add: {
    ref<Expr> left = eval(ki, 0, thr).value;
    ref<Expr> right = eval(ki, 1, thr).value;
    bindLocal(ki, thr, AddExpr::create(left, right));
    break;
  }

  case Instruction::Sub: {
    ref<Expr> left = eval(ki, 0, thr).value;
    ref<Expr> right = eval(ki, 1, thr).value;
    bindLocal(ki, thr, SubExpr::create(left, right));
    break;
  }
 
  case Instruction::Mul: {
    ref<Expr> left = eval(ki, 0, thr).value;
    ref<Expr> right = eval(ki, 1, thr).value;
    bindLocal(ki, thr, MulExpr::create(left, right));
    break;
  }

  case Instruction::UDiv: {
    ref<Expr> left = eval(ki, 0, thr).value;
    ref<Expr> right = eval(ki, 1, thr).value;
    ref<Expr> result = UDivExpr::create(left, right);
    bindLocal(ki, thr, result);
    break;
  }

  case Instruction::SDiv: {
    ref<Expr> left = eval(ki, 0, thr).value;
    ref<Expr> right = eval(ki, 1, thr).value;
    ref<Expr> result = SDivExpr::create(left, right);
    bindLocal(ki, thr, result);
    break;
  }

  case Instruction::URem: {
    ref<Expr> left = eval(ki, 0, thr).value;
    ref<Expr> right = eval(ki, 1, thr).value;
    ref<Expr> result = URemExpr::create(left, right);
    bindLocal(ki, thr, result);
    break;
  }
 
  case Instruction::SRem: {
    ref<Expr> left = eval(ki, 0, thr).value;
    ref<Expr> right = eval(ki, 1, thr).value;
    ref<Expr> result = SRemExpr::create(left, right);
    bindLocal(ki, thr, result);
    break;
  }

  case Instruction::And: {
    ref<Expr> left = eval(ki, 0, thr).value;
    ref<Expr> right = eval(ki, 1, thr).value;
    ref<Expr> result = AndExpr::create(left, right);
    bindLocal(ki, thr, result);
    break;
  }

  case Instruction::Or: {
    ref<Expr> left = eval(ki, 0, thr).value;
    ref<Expr> right = eval(ki, 1, thr).value;
    ref<Expr> result = OrExpr::create(left, right);
    bindLocal(ki, thr, result);
    break;
  }

  case Instruction::Xor: {
    ref<Expr> left = eval(ki, 0, thr).value;
    ref<Expr> right = eval(ki, 1, thr).value;
    ref<Expr> result = XorExpr::create(left, right);
    bindLocal(ki, thr, result);
    break;
  }

  case Instruction::Shl: {
    ref<Expr> left = eval(ki, 0, thr).value;
    ref<Expr> right = eval(ki, 1, thr).value;
    ref<Expr> result = ShlExpr::create(left, right);
    bindLocal(ki, thr, result);
    break;
  }

  case Instruction::LShr: {
    ref<Expr> left = eval(ki, 0, thr).value;
    ref<Expr> right = eval(ki, 1, thr).value;
    ref<Expr> result = LShrExpr::create(left, right);
    bindLocal(ki, thr, result);
    break;
  }

  case Instruction::AShr: {
    ref<Expr> left = eval(ki, 0, thr).value;
    ref<Expr> right = eval(ki, 1, thr).value;
    ref<Expr> result = AShrExpr::create(left, right);
    bindLocal(ki, thr, result);
    break;
  }

    // Compare

  case Instruction::ICmp: {
    CmpInst *ci = cast<CmpInst>(i);
    ICmpInst *ii = cast<ICmpInst>(ci);
 
    switch(ii->getPredicate()) {
    case ICmpInst::ICMP_EQ: {
      ref<Expr> left = eval(ki, 0, thr).value;
      ref<Expr> right = eval(ki, 1, thr).value;
      ref<Expr> result = EqExpr::create(left, right);
      bindLocal(ki, thr, result);
      break;
    }

    case ICmpInst::ICMP_NE: {
      ref<Expr> left = eval(ki, 0, thr).value;
      ref<Expr> right = eval(ki, 1, thr).value;
      ref<Expr> result = NeExpr::create(left, right);
      bindLocal(ki, thr, result);
      break;
    }

    case ICmpInst::ICMP_UGT: {
      ref<Expr> left = eval(ki, 0, thr).value;
      ref<Expr> right = eval(ki, 1, thr).value;
      ref<Expr> result = UgtExpr::create(left, right);
      bindLocal(ki, thr,result);
      break;
    }

    case ICmpInst::ICMP_UGE: {
      ref<Expr> left = eval(ki, 0, thr).value;
      ref<Expr> right = eval(ki, 1, thr).value;
      ref<Expr> result = UgeExpr::create(left, right);
      bindLocal(ki, thr, result);
      break;
    }

    case ICmpInst::ICMP_ULT: {
      ref<Expr> left = eval(ki, 0, thr).value;
      ref<Expr> right = eval(ki, 1, thr).value;
      ref<Expr> result = UltExpr::create(left, right);
      bindLocal(ki, thr, result);
      break;
    }

    case ICmpInst::ICMP_ULE: {
      ref<Expr> left = eval(ki, 0, thr).value;
      ref<Expr> right = eval(ki, 1, thr).value;
      ref<Expr> result = UleExpr::create(left, right);
      bindLocal(ki, thr, result);
      break;
    }

    case ICmpInst::ICMP_SGT: {
      ref<Expr> left = eval(ki, 0, thr).value;
      ref<Expr> right = eval(ki, 1, thr).value;
      ref<Expr> result = SgtExpr::create(left, right);
      bindLocal(ki, thr, result);
      break;
    }

    case ICmpInst::ICMP_SGE: {
      ref<Expr> left = eval(ki, 0, thr).value;
      ref<Expr> right = eval(ki, 1, thr).value;
      ref<Expr> result = SgeExpr::create(left, right);
      bindLocal(ki, thr, result);
      break;
    }

    case ICmpInst::ICMP_SLT: {
      ref<Expr> left = eval(ki, 0, thr).value;
      ref<Expr> right = eval(ki, 1, thr).value;
      ref<Expr> result = SltExpr::create(left, right);
      bindLocal(ki, thr, result);
      break;
    }

    case ICmpInst::ICMP_SLE: {
      ref<Expr> left = eval(ki, 0, thr).value;
      ref<Expr> right = eval(ki, 1, thr).value;
      ref<Expr> result = SleExpr::create(left, right);
      bindLocal(ki, thr, result);
      break;
    }

    default:
      terminateStateOnExecError(state, "invalid ICmp predicate");
    }
    break;
  }
 
    // Memory instructions...
#if LLVM_VERSION_CODE < LLVM_VERSION(2, 7)
  case Instruction::Malloc:
  case Instruction::Alloca: {
    AllocationInst *ai = cast<AllocationInst>(i);
#else
  case Instruction::Alloca: {
    AllocaInst *ai = cast<AllocaInst>(i);
#endif
    unsigned elementSize = 
      kmodule->targetData->getTypeStoreSize(ai->getAllocatedType());
    ref<Expr> size = Expr::createPointer(elementSize);
    if (ai->isArrayAllocation()) {
      ref<Expr> count = eval(ki, 0, thr).value;
      count = Expr::createZExtToPointerWidth(count);
      size = MulExpr::create(size, count);
    }
    bool isLocal = i->getOpcode()==Instruction::Alloca;
    executeAlloc(state, &thr, size, isLocal, ki);
    break;
  }
#if LLVM_VERSION_CODE < LLVM_VERSION(2, 7)
  case Instruction::Free: {
    executeFree(state, eval(ki, 0, thr).value);
    break;
  }
#endif

  case Instruction::Load: {
    ref<Expr> base = eval(ki, 0, thr).value;
    executeMemoryOperation(state, thr, false, base, 0, ki);
    break;
  }
  case Instruction::Store: {
    ref<Expr> base = eval(ki, 1, thr).value;
    ref<Expr> value = eval(ki, 0, thr).value;
    executeMemoryOperation(state, thr, true, base, value, 0);
    break;
  }

  case Instruction::GetElementPtr: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);
    ref<Expr> base = eval(ki, 0, thr).value;

    for (std::vector< std::pair<unsigned, uint64_t> >::iterator 
           it = kgepi->indices.begin(), ie = kgepi->indices.end(); 
         it != ie; ++it) {
      uint64_t elementSize = it->second;
      ref<Expr> index = eval(ki, it->first, thr).value;
      base = AddExpr::create(base,
                             MulExpr::create(Expr::createSExtToPointerWidth(index),
                                             Expr::createPointer(elementSize)));
    }
    if (kgepi->offset)
      base = AddExpr::create(base,
                             Expr::createPointer(kgepi->offset));
    bindLocal(ki, thr, base);
    break;
  }

    // Conversion
  case Instruction::Trunc: {
    CastInst *ci = cast<CastInst>(i);
    ref<Expr> result = ExtractExpr::create(eval(ki, 0, thr).value,
                                           0,
                                           getWidthForLLVMType(ci->getType()));
    bindLocal(ki, thr, result);
    break;
  }
  case Instruction::ZExt: {
    CastInst *ci = cast<CastInst>(i);
    ref<Expr> result = ZExtExpr::create(eval(ki, 0, thr).value,
                                        getWidthForLLVMType(ci->getType()));
    bindLocal(ki, thr, result);
    break;
  }
  case Instruction::SExt: {
    CastInst *ci = cast<CastInst>(i);
    ref<Expr> result = SExtExpr::create(eval(ki, 0, thr).value,
                                        getWidthForLLVMType(ci->getType()));
    bindLocal(ki, thr, result);
    break;
  }

  case Instruction::IntToPtr: {
    CastInst *ci = cast<CastInst>(i);
    Expr::Width pType = getWidthForLLVMType(ci->getType());
    ref<Expr> arg = eval(ki, 0, thr).value;
    bindLocal(ki, thr, ZExtExpr::create(arg, pType));
    break;
  } 
  case Instruction::PtrToInt: {
    CastInst *ci = cast<CastInst>(i);
    Expr::Width iType = getWidthForLLVMType(ci->getType());
    ref<Expr> arg = eval(ki, 0, thr).value;
    bindLocal(ki, thr, ZExtExpr::create(arg, iType));
    break;
  }

  case Instruction::BitCast: {
    ref<Expr> result = eval(ki, 0, thr).value;
    bindLocal(ki, thr, result);
    break;
  }

    // Floating point instructions

  case Instruction::FAdd: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, thr).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, thr).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FAdd operation");

    llvm::APFloat Res(left->getAPValue());
    Res.add(APFloat(right->getAPValue()), APFloat::rmNearestTiesToEven);
    bindLocal(ki, thr, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }

  case Instruction::FSub: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, thr).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, thr).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FSub operation");

    llvm::APFloat Res(left->getAPValue());
    Res.subtract(APFloat(right->getAPValue()), APFloat::rmNearestTiesToEven);
    bindLocal(ki, thr, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }
 
  case Instruction::FMul: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, thr).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, thr).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FMul operation");

    llvm::APFloat Res(left->getAPValue());
    Res.multiply(APFloat(right->getAPValue()), APFloat::rmNearestTiesToEven);
    bindLocal(ki, thr, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }

  case Instruction::FDiv: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, thr).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, thr).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FDiv operation");

    llvm::APFloat Res(left->getAPValue());
    Res.divide(APFloat(right->getAPValue()), APFloat::rmNearestTiesToEven);
    bindLocal(ki, thr, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }

  case Instruction::FRem: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, thr).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, thr).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FRem operation");

    llvm::APFloat Res(left->getAPValue());
    Res.mod(APFloat(right->getAPValue()), APFloat::rmNearestTiesToEven);
    bindLocal(ki, thr, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }

  case Instruction::FPTrunc: {
    FPTruncInst *fi = cast<FPTruncInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, thr).value,
                                       "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || resultType > arg->getWidth())
      return terminateStateOnExecError(state, "Unsupported FPTrunc operation");

    llvm::APFloat Res(arg->getAPValue());
    bool losesInfo = false;
    Res.convert(*fpWidthToSemantics(resultType),
                llvm::APFloat::rmNearestTiesToEven,
                &losesInfo);
    bindLocal(ki, thr, ConstantExpr::alloc(Res));
    break;
  }

  case Instruction::FPExt: {
    FPExtInst *fi = cast<FPExtInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, thr).value,
                                        "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || arg->getWidth() > resultType)
      return terminateStateOnExecError(state, "Unsupported FPExt operation");

    llvm::APFloat Res(arg->getAPValue());
    bool losesInfo = false;
    Res.convert(*fpWidthToSemantics(resultType),
                llvm::APFloat::rmNearestTiesToEven,
                &losesInfo);
    bindLocal(ki, thr, ConstantExpr::alloc(Res));
    break;
  }

  case Instruction::FPToUI: {
    FPToUIInst *fi = cast<FPToUIInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, thr).value,
                                       "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || resultType > 64)
      return terminateStateOnExecError(state, "Unsupported FPToUI operation");

    llvm::APFloat Arg(arg->getAPValue());
    uint64_t value = 0;
    bool isExact = true;
    Arg.convertToInteger(&value, resultType, false,
                         llvm::APFloat::rmTowardZero, &isExact);
    bindLocal(ki, thr, ConstantExpr::alloc(value, resultType));
    break;
  }

  case Instruction::FPToSI: {
    FPToSIInst *fi = cast<FPToSIInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, thr).value,
                                       "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || resultType > 64)
      return terminateStateOnExecError(state, "Unsupported FPToSI operation");

    llvm::APFloat Arg(arg->getAPValue());
    uint64_t value = 0;
    bool isExact = true;
    Arg.convertToInteger(&value, resultType, false,
                         llvm::APFloat::rmTowardZero, &isExact);
    bindLocal(ki, thr, ConstantExpr::alloc(value, resultType));
    break;
  }

  case Instruction::UIToFP: {
    UIToFPInst *fi = cast<UIToFPInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, thr).value,
                                       "floating point");
    const llvm::fltSemantics *semantics = fpWidthToSemantics(resultType);
    if (!semantics)
      return terminateStateOnExecError(state, "Unsupported UIToFP operation");
    llvm::APFloat f(*semantics, 0);
    f.convertFromAPInt(arg->getAPValue(), false,
                       llvm::APFloat::rmNearestTiesToEven);

    bindLocal(ki, thr, ConstantExpr::alloc(f));
    break;
  }

  case Instruction::SIToFP: {
    SIToFPInst *fi = cast<SIToFPInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, thr).value,
                                       "floating point");
    const llvm::fltSemantics *semantics = fpWidthToSemantics(resultType);
    if (!semantics)
      return terminateStateOnExecError(state, "Unsupported SIToFP operation");
    llvm::APFloat f(*semantics, 0);
    f.convertFromAPInt(arg->getAPValue(), true,
                       llvm::APFloat::rmNearestTiesToEven);

    bindLocal(ki, thr, ConstantExpr::alloc(f));
    break;
  }

  case Instruction::FCmp: {
    FCmpInst *fi = cast<FCmpInst>(i);
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, thr).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, thr).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FCmp operation");

    APFloat LHS(left->getAPValue());
    APFloat RHS(right->getAPValue());
    APFloat::cmpResult CmpRes = LHS.compare(RHS);

    bool Result = false;
    switch( fi->getPredicate() ) {
      // Predicates which only care about whether or not the operands are NaNs.
    case FCmpInst::FCMP_ORD:
      Result = CmpRes != APFloat::cmpUnordered;
      break;

    case FCmpInst::FCMP_UNO:
      Result = CmpRes == APFloat::cmpUnordered;
      break;

      // Ordered comparisons return false if either operand is NaN.  Unordered
      // comparisons return true if either operand is NaN.
    case FCmpInst::FCMP_UEQ:
      if (CmpRes == APFloat::cmpUnordered) {
        Result = true;
        break;
      }
    case FCmpInst::FCMP_OEQ:
      Result = CmpRes == APFloat::cmpEqual;
      break;

    case FCmpInst::FCMP_UGT:
      if (CmpRes == APFloat::cmpUnordered) {
        Result = true;
        break;
      }
    case FCmpInst::FCMP_OGT:
      Result = CmpRes == APFloat::cmpGreaterThan;
      break;

    case FCmpInst::FCMP_UGE:
      if (CmpRes == APFloat::cmpUnordered) {
        Result = true;
        break;
      }
    case FCmpInst::FCMP_OGE:
      Result = CmpRes == APFloat::cmpGreaterThan || CmpRes == APFloat::cmpEqual;
      break;

    case FCmpInst::FCMP_ULT:
      if (CmpRes == APFloat::cmpUnordered) {
        Result = true;
        break;
      }
    case FCmpInst::FCMP_OLT:
      Result = CmpRes == APFloat::cmpLessThan;
      break;

    case FCmpInst::FCMP_ULE:
      if (CmpRes == APFloat::cmpUnordered) {
        Result = true;
        break;
      }
    case FCmpInst::FCMP_OLE:
      Result = CmpRes == APFloat::cmpLessThan || CmpRes == APFloat::cmpEqual;
      break;

    case FCmpInst::FCMP_UNE:
      Result = CmpRes == APFloat::cmpUnordered || CmpRes != APFloat::cmpEqual;
      break;
    case FCmpInst::FCMP_ONE:
      Result = CmpRes != APFloat::cmpUnordered && CmpRes != APFloat::cmpEqual;
      break;

    default:
      assert(0 && "Invalid FCMP predicate!");
    case FCmpInst::FCMP_FALSE:
      Result = false;
      break;
    case FCmpInst::FCMP_TRUE:
      Result = true;
      break;
    }

    bindLocal(ki, thr, ConstantExpr::alloc(Result, Expr::Bool));
    break;
  }
  case Instruction::InsertValue: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);

    ref<Expr> agg = eval(ki, 0, thr).value;
    ref<Expr> val = eval(ki, 1, thr).value;

    ref<Expr> l = NULL, r = NULL;
    unsigned lOffset = kgepi->offset*8, rOffset = kgepi->offset*8 + val->getWidth();

    if (lOffset > 0)
      l = ExtractExpr::create(agg, 0, lOffset);
    if (rOffset < agg->getWidth())
      r = ExtractExpr::create(agg, rOffset, agg->getWidth() - rOffset);

    ref<Expr> result;
    if (!l.isNull() && !r.isNull())
      result = ConcatExpr::create(r, ConcatExpr::create(val, l));
    else if (!l.isNull())
      result = ConcatExpr::create(val, l);
    else if (!r.isNull())
      result = ConcatExpr::create(r, val);
    else
      result = val;

    bindLocal(ki, thr, result);
    break;
  }
  case Instruction::ExtractValue: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);

    ref<Expr> agg = eval(ki, 0, thr).value;

    ref<Expr> result = ExtractExpr::create(agg, kgepi->offset*8, getWidthForLLVMType(i->getType()));

    bindLocal(ki, thr, result);
    break;
  }
 
    // Other instructions...
    // Unhandled
  case Instruction::ExtractElement:
  case Instruction::InsertElement:
  case Instruction::ShuffleVector:
    terminateStateOnError(state, "XXX vector instructions unhandled",
                          "xxx.err");
    break;
 
  default:
    terminateStateOnExecError(state, "illegal instruction");
    break;
  }
}

void Executor::updateStates(ExecutionState *current) {
  if (UsePathSlicer) {
    /* Discard current state if it is already pruned. Some states may still be put back into
    the loop due to some specific processing mechanism of KLEE, such as executeAlloc():
    the forked state is killed due to huge size in klee, and the parent is pruned by the path
    slicer, but it is still in the state pair and may be put back into searcher. This return below
    is to prevent this.
    */
    if (current && current->isPruned && !MarkPrunedOnly) {
      std::cerr << "Executor::updateStates curState " << (void *)current
        << " is pruned and discarded." << endl;
      return;
    }
  }
  
  if (searcher) {
    searcher->update(current, addedStates, removedStates);
  }
  
  states.insert(addedStates.begin(), addedStates.end());
  addedStates.clear();
  
  for (std::set<ExecutionState*>::iterator
         it = removedStates.begin(), ie = removedStates.end();
       it != ie; ++it) {
    ExecutionState *es = *it;
    std::set<ExecutionState*>::iterator it2 = states.find(es);
    assert(it2!=states.end());
    states.erase(it2);
    std::map<ExecutionState*, std::vector<SeedInfo> >::iterator it3 = 
      seedMap.find(es);
    if (it3 != seedMap.end())
      seedMap.erase(it3);
    processTree->remove(es->ptreeNode);
    if (UsePathSlicer) {
      pathSlicer->clearPath((void *)es);
      brTree.removeState(es);
    }
    delete es;
  }
  removedStates.clear();
}

template <typename TypeIt>
void Executor::computeOffsets(KGEPInstruction *kgepi, TypeIt ib, TypeIt ie) {
  ref<ConstantExpr> constantOffset =
    ConstantExpr::alloc(0, Context::get().getPointerWidth());
  uint64_t index = 1;
  for (TypeIt ii = ib; ii != ie; ++ii) {
    if (LLVM_TYPE_Q StructType *st = dyn_cast<StructType>(*ii)) {
      const StructLayout *sl = kmodule->targetData->getStructLayout(st);
      const ConstantInt *ci = cast<ConstantInt>(ii.getOperand());
      uint64_t addend = sl->getElementOffset((unsigned) ci->getZExtValue());
      constantOffset = constantOffset->Add(ConstantExpr::alloc(addend,
                                                               Context::get().getPointerWidth()));
    } else {
      const SequentialType *set = cast<SequentialType>(*ii);
      uint64_t elementSize = 
        kmodule->targetData->getTypeStoreSize(set->getElementType());
      Value *operand = ii.getOperand();
      if (Constant *c = dyn_cast<Constant>(operand)) {
        ref<ConstantExpr> index = 
          evalConstant(c)->SExt(Context::get().getPointerWidth());
        ref<ConstantExpr> addend = 
          index->Mul(ConstantExpr::alloc(elementSize,
                                         Context::get().getPointerWidth()));
        constantOffset = constantOffset->Add(addend);
      } else {
        kgepi->indices.push_back(std::make_pair(index, elementSize));
      }
    }
    index++;
  }
  kgepi->offset = constantOffset->getZExtValue();
}

void Executor::bindInstructionConstants(KInstruction *KI) {
  KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(KI);

  if (GetElementPtrInst *gepi = dyn_cast<GetElementPtrInst>(KI->inst)) {
    computeOffsets(kgepi, gep_type_begin(gepi), gep_type_end(gepi));
  } else if (InsertValueInst *ivi = dyn_cast<InsertValueInst>(KI->inst)) {
    computeOffsets(kgepi, iv_type_begin(ivi), iv_type_end(ivi));
    assert(kgepi->indices.empty() && "InsertValue constant offset expected");
  } else if (ExtractValueInst *evi = dyn_cast<ExtractValueInst>(KI->inst)) {
    computeOffsets(kgepi, ev_type_begin(evi), ev_type_end(evi));
    assert(kgepi->indices.empty() && "ExtractValue constant offset expected");
  }
}

void Executor::bindModuleConstants() {
  for (std::vector<KFunction*>::iterator it = kmodule->functions.begin(), 
         ie = kmodule->functions.end(); it != ie; ++it) {
    KFunction *kf = *it;
    for (unsigned i=0; i<kf->numInstructions; ++i)
      bindInstructionConstants(kf->instructions[i]);
  }

  kmodule->constantTable = new Cell[kmodule->constants.size()];
  for (unsigned i=0; i<kmodule->constants.size(); ++i) {
    Cell &c = kmodule->constantTable[i];
    c.value = evalConstant(kmodule->constants[i]);
  }
}

void Executor::runNextInstruction(ExecutionState &state)
{
    // Round-robin method
    ExecutionState::threads_ty::iterator it;

    int min_tid = -1;
    int next_tid = -1;
    for (it = state.threads.begin(); it != state.threads.end(); it++)
    {
        int tid = (*it).first;
        if (state.blockedThreads.find(tid) == state.blockedThreads.end())
        {
            if (tid > state.lastTid)
            {
                if (next_tid == -1 || next_tid > tid)
                {
                    next_tid = tid;
                }
            }
            if (min_tid == -1 || min_tid > tid)
            {
                min_tid = tid;
            }
        }
    }

    if (next_tid == -1)
    {
        next_tid = min_tid;
    }

    KInstruction *inst = state.getThread(next_tid)->pc;
    stepInstruction(state, next_tid);
    executeInstruction(state, next_tid, inst);

/*    for (std::set<int>::iterator it = next_set.begin(); it != next_set.end(); it++)
    {
        assert(state.blockedThreads.find(*it) != state.blockedThreads.end());
        state.blockedThreads.erase(*it);
    }*/

    state.lastTid = next_tid;
 
    /* 
     * check all threads for sync operations
     * only allow the thread with min. tid
     */
/*    ExecutionState::threads_ty::iterator it;
    int min_sync_tid = -1;
    std::set<int> blocked_threads;
    for (it = state.threads.begin(); it != state.threads.end(); it++)
    {
        KInstruction *ki = (*it).second.pc;

        if (isSyncOperation(ki))
        {
            if (min_sync_tid == -1 || min_sync_tid > (*it).first)
            {
                if (min_sync_tid != -1)
                    blocked_threads.insert(min_sync_tid);
                min_sync_tid = (*it).first;
            }
        }
    }

    // step over!
    for (it = state.threads.begin(); it != state.threads.end(); it++)
    {
        int tid = (*it).first;
        KInstruction *ki = (*it).second.pc;
        if (blocked_threads.find(tid) != blocked_threads.end())
        {
            stepInstruction(state, tid);
            executeInstruction(state, tid, (*it).second.pc);
        }
    }*/

/*    KInstruction *ki = state.pc;
    stepInstruction(state);

    executeInstruction(state, ki);*/

}

void Executor::run(ExecutionState &initialState) {
  bindModuleConstants();

  // Delay init till now so that ticks don't accrue during
  // optimization and such.
  initTimers();

  states.insert(&initialState);

  if (usingSeeds) {
    std::vector<SeedInfo> &v = seedMap[&initialState];
    
    for (std::vector<KTest*>::const_iterator it = usingSeeds->begin(), 
           ie = usingSeeds->end(); it != ie; ++it)
      v.push_back(SeedInfo(*it));

    int lastNumSeeds = usingSeeds->size()+10;
    double lastTime, startTime = lastTime = util::getWallTime();
    ExecutionState *lastState = 0;
    while (!seedMap.empty()) {
      if (haltExecution) goto dump;

      std::map<ExecutionState*, std::vector<SeedInfo> >::iterator it = 
        seedMap.upper_bound(lastState);
      if (it == seedMap.end())
        it = seedMap.begin();
      lastState = it->first;
      unsigned numSeeds = it->second.size();
      ExecutionState &state = *lastState;
      runNextInstruction(state);
/*      KInstruction *ki = state.pc;
      stepInstruction(state);

      executeInstruction(state, ki);*/
      processTimers(&state, MaxInstructionTime * numSeeds);
      updateStates(&state);

      if ((stats::instructions % 1000) == 0) {
        int numSeeds = 0, numStates = 0;
        for (std::map<ExecutionState*, std::vector<SeedInfo> >::iterator
               it = seedMap.begin(), ie = seedMap.end();
             it != ie; ++it) {
          numSeeds += it->second.size();
          numStates++;
        }
        double time = util::getWallTime();
        if (SeedTime>0. && time > startTime + SeedTime) {
          klee_warning("seed time expired, %d seeds remain over %d states",
                       numSeeds, numStates);
          break;
        } else if (numSeeds<=lastNumSeeds-10 ||
                   time >= lastTime+10) {
          lastTime = time;
          lastNumSeeds = numSeeds;          
          klee_message("%d seeds remaining over: %d states", 
                       numSeeds, numStates);
        }
      }
    }

    klee_message("seeding done (%d states remain)", (int) states.size());

    // XXX total hack, just because I like non uniform better but want
    // seed results to be equally weighted.
    for (std::set<ExecutionState*>::iterator
           it = states.begin(), ie = states.end();
         it != ie; ++it) {
      (*it)->weight = 1.;
    }

    if (OnlySeed)
      goto dump;
  }

  searcher = constructUserSearcher(*this);

  searcher->update(0, states, std::set<ExecutionState*>());

  while (!states.empty() && !haltExecution) {
    ExecutionState &state = searcher->selectState();

    runNextInstruction(state);
  
    processTimers(&state, MaxInstructionTime);

    if (MaxMemory) {
      if ((stats::instructions & 0xFFFF) == 0) {
        // We need to avoid calling GetMallocUsage() often because it
        // is O(elts on freelist). This is really bad since we start
        // to pummel the freelist once we hit the memory cap.
        unsigned mbs = sys::Process::GetTotalMemoryUsage() >> 20;
        
        if (mbs > MaxMemory) {
          if (mbs > MaxMemory + 100) {
            // just guess at how many to kill
            unsigned numStates = states.size();
            unsigned toKill = std::max(1U, numStates - numStates*MaxMemory/mbs);

            if (MaxMemoryInhibit)
              klee_warning("killing %d states (over memory cap)",
                           toKill);

            std::vector<ExecutionState*> arr(states.begin(), states.end());
            for (unsigned i=0,N=arr.size(); N && i<toKill; ++i,--N) {
              unsigned idx = rand() % N;

              // Make two pulls to try and not hit a state that
              // covered new code.
              if (arr[idx]->coveredNew)
                idx = rand() % N;

              std::swap(arr[idx], arr[N-1]);
              terminateStateEarly(*arr[N-1], "memory limit");
            }
          }
          atMemoryLimit = true;
        } else {
          atMemoryLimit = false;
        }
      }
    }

    updateStates(&state);
  }

  delete searcher;
  searcher = 0;
  
 dump:
  if (DumpStatesOnHalt && !states.empty()) {
    std::cerr << "KLEE: halting execution, dumping remaining states\n";
    for (std::set<ExecutionState*>::iterator
           it = states.begin(), ie = states.end();
         it != ie; ++it) {
      ExecutionState &state = **it;
      for (ExecutionState::threads_ty::iterator it = state.threads.begin();
              it != state.threads.end(); it++)
      {
          stepInstruction(state, (*it).first); // keep stats rolling
      }
      terminateStateEarly(state, "execution halting");
    }
    updateStates(0);
  }
}

std::string Executor::getAddressInfo(ExecutionState &state, 
                                     ref<Expr> address) const{
  std::ostringstream info;
  info << "\taddress: " << address << "\n";
  uint64_t example;
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(address)) {
    example = CE->getZExtValue();
  } else {
    ref<ConstantExpr> value;
    bool success = solver->getValue(state, address, value);
    assert(success && "FIXME: Unhandled solver failure");
    (void) success;
    example = value->getZExtValue();
    info << "\texample: " << example << "\n";
    std::pair< ref<Expr>, ref<Expr> > res = solver->getRange(state, address);
    info << "\trange: [" << res.first << ", " << res.second <<"]\n";
  }
  
  MemoryObject hack((unsigned) example);    
  MemoryMap::iterator lower = state.addressSpace.objects.upper_bound(&hack);
  info << "\tnext: ";
  if (lower==state.addressSpace.objects.end()) {
    info << "none\n";
  } else {
    const MemoryObject *mo = lower->first;
    std::string alloc_info;
    mo->getAllocInfo(alloc_info);
    info << "object at " << mo->address
         << " of size " << mo->size << "\n"
         << "\t\t" << alloc_info << "\n";
  }
  if (lower!=state.addressSpace.objects.begin()) {
    --lower;
    info << "\tprev: ";
    if (lower==state.addressSpace.objects.end()) {
      info << "none\n";
    } else {
      const MemoryObject *mo = lower->first;
      std::string alloc_info;
      mo->getAllocInfo(alloc_info);
      info << "object at " << mo->address 
           << " of size " << mo->size << "\n"
           << "\t\t" << alloc_info << "\n";
    }
  }

  return info.str();
}

void Executor::terminateState(ExecutionState &state) {
  if (UsePathSlicer)
    pathSlicer->collectStatesStat((void *)&state);
  
  if (replayOut && replayPosition!=replayOut->numObjects) {
    klee_warning_once(replayOut, 
                      "replay did not consume all objects in test input.");
  }

  interpreterHandler->incPathsExplored();

  std::set<ExecutionState*>::iterator it = addedStates.find(&state);
  if (it==addedStates.end()) {
    state.stepBack();
    removedStates.insert(&state);
  } else {
    // never reached searcher, just delete immediately
    std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it3 = 
      seedMap.find(&state);
    if (it3 != seedMap.end())
      seedMap.erase(it3);
    addedStates.erase(it);
    processTree->remove(state.ptreeNode);
    if (UsePathSlicer) {
      pathSlicer->clearPath((void *)&state);
      brTree.removeState(&state);
    }
    delete &state;
  }
}

void Executor::terminateStateEarly(ExecutionState &state, 
                                   const Twine &message) {
  if (!OnlyOutputStatesCoveringNew || state.coveredNew ||
      (AlwaysOutputSeeds && seedMap.count(&state)))
    interpreterHandler->processTestCase(state, (message + "\n").str().c_str(),
                                        "early");
  terminateState(state);
}

void Executor::terminateStateOnExit(ExecutionState &state) {
  if (state.localCheckers.report(&state) & Checker::ERROR)
  {
      if (UsePathSlicer) {
        pathSlicer->recordCheckerResult((void *)&state, Checker::OK,
          Checker::ERROR, interpreterHandler->getNumTestCases());
        // Do slicing and pruning when a checker error is found.
        slicingAndPruneStates(state);
      }
      state.localCheckers.clear();
      terminateStateOnError(state, "checker reported error", "check.err");
      return;
  }

  // Still do slicing and pruning when a checker error is not found.
  if (UsePathSlicer)
    slicingAndPruneStates(state);

  if (!OnlyOutputStatesCoveringNew || state.coveredNew || 
      (AlwaysOutputSeeds && seedMap.count(&state)))
    interpreterHandler->processTestCase(state, 0, 0);
  terminateState(state);
}

void Executor::terminateStateOnError(ExecutionState &state,
                                     const llvm::Twine &messaget,
                                     const char *suffix,
                                     const llvm::Twine &info) {
  std::string message = messaget.str();
  fprintf(stderr, "Executor::terminateStateOnError state %p, message (%s).\n", 
    (void *)&state, message.c_str());
  printKLEECallStack(state);
  static std::set< std::pair<Instruction*, std::string> > emittedErrors;
  KInstruction *prevPC = (*state.threads.find(state.lastTid)).second.prevPC;
  const InstructionInfo &ii = *prevPC->info;
  
  if (EmitAllErrors ||
      emittedErrors.insert(std::make_pair(prevPC->inst, message)).second) {
    if (ii.file != "") {
      klee_message("ERROR: %s:%d: %s", ii.file.c_str(), ii.line, message.c_str());
    } else {
      klee_message("ERROR: %s", message.c_str());
    }
    if (!EmitAllErrors)
      klee_message("NOTE: now ignoring this error at this location");
    
    std::ostringstream msg;
    msg << "Error: " << message << "\n";
    if (ii.file != "") {
      msg << "File: " << ii.file << "\n";
      msg << "Line: " << ii.line << "\n";
    }
    msg << "Stack: \n";
    state.dumpStack(msg);

    std::string info_str = info.str();
    if (info_str != "")
      msg << "Info: \n" << info_str;
    interpreterHandler->processTestCase(state, msg.str().c_str(), suffix);
  }
    
  terminateState(state);
}

// XXX shoot me
static const char *okExternalsList[] = { "printf", 
                                         "fprintf", 
                                         "puts",
                                         "getpid" };
static std::set<std::string> okExternals(okExternalsList,
                                         okExternalsList + 
                                         (sizeof(okExternalsList)/sizeof(okExternalsList[0])));

void Executor::emulateSync(ExecutionState &state,
        ThreadState &thr,
        KInstruction *target,
        Function *function,
        std::vector< ref<Expr> > &arguments,
        uint64_t *args) {
  std::string name = function->getName();
}

void Executor::callExternalFunction(ExecutionState &state,
                                    ThreadState &thr,
                                    KInstruction *target,
                                    Function *function,
                                    std::vector< ref<Expr> > &arguments) {
  
   // check if specialFunctionHandler wants it
  bool handleSpecial = specialFunctionHandler->handle(state, thr, function, target, arguments);
  if (handleSpecial)
      if (!globalCheckers.wantConcrete(state, thr, target, function, arguments) &&
              !state.localCheckers.wantConcrete(state, thr, target, function, arguments))
      {
          if (!UsePathSlicer || curInstrIsInternal(target)) {
            Checker::Result globalResult = 
              globalCheckers.onCall(state, thr, target, function, arguments, NULL);
            Checker::Result localResult = 
              state.localCheckers.onCall(state, thr, target, function, arguments, NULL);
            if (UsePathSlicer)
              pathSlicer->recordCheckerResult((void *)&state, globalResult,
                localResult, interpreterHandler->getNumTestCases());
            if (globalResult & Checker::ERROR || localResult & Checker::ERROR)
                terminateStateOnError(state, "checker reported error", "check.err");
          }
          return;
      }

  if (!handleSpecial)
  {
      if (NoExternals && !okExternals.count(function->getName())) {
          std::cerr << "KLEE:ERROR: Calling not-OK external function : " 
              << function->getNameStr() << "\n";
          terminateStateOnError(state, "externals disallowed", "user.err");
          return;
      }
  }

  // normal external function handling path
  // allocate 128 bits for each argument (+return value) to support fp80's;
  // we could iterate through all the arguments first and determine the exact
  // size we need, but this is faster, and the memory usage isn't significant.
  uint64_t *args = (uint64_t*) alloca(2*sizeof(*args) * (arguments.size() + 1));
  memset(args, 0, 2 * sizeof(*args) * (arguments.size() + 1));
  unsigned wordIndex = 2;
  for (std::vector<ref<Expr> >::iterator ai = arguments.begin(), 
       ae = arguments.end(); ai!=ae; ++ai) {
    if (AllowExternalSymCalls || handleSpecial) { // don't bother checking uniqueness
      ref<ConstantExpr> ce;
      bool success = solver->getValue(state, *ai, ce);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      ce->toMemory(&args[wordIndex]);
      wordIndex += (ce->getWidth()+63)/64;
    } else {
      ref<Expr> arg = toUnique(state, *ai);
      if (ConstantExpr *ce = dyn_cast<ConstantExpr>(arg)) {
        // XXX kick toMemory functions from here
        ce->toMemory(&args[wordIndex]);
        wordIndex += (ce->getWidth()+63)/64;
      } else {
          terminateStateOnExecError(state, 
                  "external call with symbolic argument: " + 
                  function->getName());
          return;
      }
    }
  }

  state.addressSpace.copyOutConcretes();

  if (handleSpecial)
  {
      if (!UsePathSlicer || curInstrIsInternal(target)) {
        ref<Expr> ret_orig = getDestCell(thr, target).value;
        if (ret_orig.get()) // ignore void return
        {
          ref<Expr> ret = toUnique(state, getDestCell(thr, target).value);
          if (ConstantExpr *ce = dyn_cast<ConstantExpr>(ret)) {
            ce->toMemory(&args[0]);
          } else {
            std::cerr << "warning: failed to concretize return value." << std::endl;
            ref<ConstantExpr> rce;
            if (solver->getValue(state, ret, rce)) {
                rce->toMemory(&args[0]);
            } else {
                args[0] = args[1] = 0;
            }
          }
        }
        Checker::Result globalResult =
          globalCheckers.onCall(state, thr, target, function, arguments, args);
        Checker::Result localResult =
          state.localCheckers.onCall(state, thr, target, function, arguments, args);
        if (UsePathSlicer)
          pathSlicer->recordCheckerResult((void *)&state, globalResult,
            localResult, interpreterHandler->getNumTestCases());
        if (globalResult & Checker::ERROR || localResult & Checker::ERROR)
            terminateStateOnError(state, "checker reported error", "check.err");
      }
      return;
  }

  if (!SuppressExternalWarnings) {
    std::ostringstream os;
    os << "calling external: " << function->getName().str() << "(";
    for (unsigned i=0; i<arguments.size(); i++) {
      os << arguments[i];
      if (i != arguments.size()-1)
	os << ", ";
    }
    os << ")";
    
    if (AllExternalWarnings)
      klee_warning("%s", os.str().c_str());
    else
      klee_warning_once(function, "%s", os.str().c_str());
  }

  // Emulate effects of pthread functions
  //
  emulateSync(state, thr, target, function, arguments, args);
  
  bool success = externalDispatcher->executeCall(function, target->inst, args);
  if (!success) {
    terminateStateOnError(state, "failed external call: " + function->getName(),
                          "external.err");
    return;
  }

  if (!state.addressSpace.copyInConcretes()) {
    terminateStateOnError(state, "external modified read-only object",
                          "external.err");
    return;
  }

  if (!UsePathSlicer || curInstrIsInternal(target)) {
    Checker::Result globalResult = 
      globalCheckers.onCall(state, thr, target, function, arguments, args);
    Checker::Result localResult =
      state.localCheckers.onCall(state, thr, target, function, arguments, args);
    if (UsePathSlicer)
      pathSlicer->recordCheckerResult((void *)&state, globalResult,
        localResult, interpreterHandler->getNumTestCases());
    if (globalResult & Checker::ERROR || localResult & Checker::ERROR)
        terminateStateOnError(state, "checker reported error", "check.err");
  }

  LLVM_TYPE_Q Type *resultType = target->inst->getType();
  if (resultType != Type::getVoidTy(getGlobalContext())) {
    ref<Expr> e = ConstantExpr::fromMemory((void*) args, 
                                           getWidthForLLVMType(resultType));
    bindLocal(target, thr, e);
  }
}

/***/

ref<Expr> Executor::replaceReadWithSymbolic(ExecutionState &state, 
                                            ref<Expr> e) {
  unsigned n = interpreterOpts.MakeConcreteSymbolic;
  if (!n || replayOut || replayPath)
    return e;

  // right now, we don't replace symbolics (is there any reason too?)
  if (!isa<ConstantExpr>(e))
    return e;

  if (n != 1 && random() %  n)
    return e;

  // create a new fresh location, assert it is equal to concrete value in e
  // and return it.
  
  static unsigned id;
  const Array *array = new Array("rrws_arr" + llvm::utostr(++id), 
                                 Expr::getMinBytesForWidth(e->getWidth()));
  ref<Expr> res = Expr::createTempRead(array, e->getWidth());
  ref<Expr> eq = NotOptimizedExpr::create(EqExpr::create(e, res));
  std::cerr << "Making symbolic: " << eq << "\n";
  state.addConstraint(eq);
  return res;
}

ObjectState *Executor::bindObjectInState(ExecutionState &state, 
                                         ThreadState *thr,
                                         const MemoryObject *mo,
                                         bool isLocal,
                                         const Array *array) {
  ObjectState *os = array ? new ObjectState(mo, array) : new ObjectState(mo);
  state.addressSpace.bindObject(mo, os);

  // Its possible that multiple bindings of the same mo in the state
  // will put multiple copies on this list, but it doesn't really
  // matter because all we use this list for is to unbind the object
  // on function return.
  if (isLocal)
    thr->stack.back().allocas.push_back(mo);

  return os;
}

void Executor::executeAlloc(ExecutionState &state,
                            ThreadState *thr,
                            ref<Expr> size,
                            bool isLocal,
                            KInstruction *target,
                            bool zeroMemory,
                            const ObjectState *reallocFrom) {
  size = toUnique(state, size);
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(size)) {
    MemoryObject *mo = memory->allocate(CE->getZExtValue(), isLocal, false, 
                                        thr->prevPC->inst);
    if (!mo) {
      bindLocal(target, *thr, 
                ConstantExpr::alloc(0, Context::get().getPointerWidth()));
    } else {
      ObjectState *os = bindObjectInState(state, thr, mo, isLocal);
      if (zeroMemory) {
        os->initializeToZero();
      } else {
        os->initializeToRandom();
      }
      bindLocal(target, *thr, mo->getBaseExpr());
      
      if (reallocFrom) {
        unsigned count = std::min(reallocFrom->size, os->size);
        for (unsigned i=0; i<count; i++)
          os->write(i, reallocFrom->read8(i));
        state.addressSpace.unbindObject(reallocFrom->getObject());
      }
    }
  } else {
    // XXX For now we just pick a size. Ideally we would support
    // symbolic sizes fully but even if we don't it would be better to
    // "smartly" pick a value, for example we could fork and pick the
    // min and max values and perhaps some intermediate (reasonable
    // value).
    // 
    // It would also be nice to recognize the case when size has
    // exactly two values and just fork (but we need to get rid of
    // return argument first). This shows up in pcre when llvm
    // collapses the size expression with a select.

    ref<ConstantExpr> example;
    bool success = solver->getValue(state, size, example);
    assert(success && "FIXME: Unhandled solver failure");
    (void) success;
    
#ifdef FORK_CALL_DEBUG
    std::cerr << "symbolic alloc. incoming state: " << &state << std::endl;
#endif
    // Try and start with a small example.
    Expr::Width W = example->getWidth();
    while (example->Ugt(ConstantExpr::alloc(128, W))->isTrue()) {
      ref<ConstantExpr> tmp = example->LShr(ConstantExpr::alloc(1, W));
      bool res;
      bool success = solver->mayBeTrue(state, EqExpr::create(tmp, size), res);
      assert(success && "FIXME: Unhandled solver failure");      
      (void) success;
      if (!res)
        break;
      example = tmp;
    }

    int tid = thr->id;
    StatePair fixedSize = fork(state, tid, EqExpr::create(example, size), true);

#ifdef FORK_CALL_DEBUG
    std::cerr << "after first fork: " << fixedSize.first << "," << fixedSize.second << std::endl;
#endif
    
    if (fixedSize.second) { 
      // Check for exactly two values
      ref<ConstantExpr> tmp;
      bool success = solver->getValue(*fixedSize.second, size, tmp);
      assert(success && "FIXME: Unhandled solver failure");      
      (void) success;
      bool res;
      success = solver->mustBeTrue(*fixedSize.second, 
                                   EqExpr::create(tmp, size),
                                   res);
      assert(success && "FIXME: Unhandled solver failure");      
      (void) success;
      if (res) {
        executeAlloc(*fixedSize.second, fixedSize.second->getThread(tid), tmp, isLocal,
                     target, zeroMemory, reallocFrom);
        if (fixedSize.second != &state)
            callChecker(*fixedSize.second, target, tid);
      } else {
        int tid = thr->id;
        // See if a *really* big value is possible. If so assume
        // malloc will fail for it, so lets fork and return 0.
        StatePair hugeSize = 
          fork(*fixedSize.second, tid,
               UltExpr::create(ConstantExpr::alloc(1<<31, W), size), 
               true);
#ifdef FORK_CALL_DEBUG
        std::cerr << "after second fork: " << hugeSize.first << "," << hugeSize.second << std::endl;
#endif
        if (hugeSize.first) {
          klee_message("NOTE: found huge malloc, returning 0");
          bindLocal(target, *hugeSize.first->getThread(tid), 
                    ConstantExpr::alloc(0, Context::get().getPointerWidth()));
          if (hugeSize.first != &state)
              callChecker(*hugeSize.first, target, tid);
        }
        
        if (hugeSize.second) {
          std::ostringstream info;
          ExprPPrinter::printOne(info, "  size expr", size);
          info << "  concretization : " << example << "\n";
          info << "  unbound example: " << tmp << "\n";
          terminateStateOnError(*hugeSize.second, 
                                "concretized symbolic size", 
                                "model.err", 
                                info.str());
        }
      }
    }

    if (fixedSize.first) { // can be zero when fork fails
      executeAlloc(*fixedSize.first, fixedSize.first->getThread(tid), example, isLocal, 
                   target, zeroMemory, reallocFrom);
      if (fixedSize.first != &state)
          callChecker(*fixedSize.first, target, tid);
    }
  }
}

bool Executor::callChecker(ExecutionState &newState, KInstruction *target, int tid) {
    if (!UsePathSlicer || curInstrIsInternal(target)) {
#ifdef FORK_CALL_DEBUG
        std::cerr << "doing extra onCall() on state " << &newState << std::endl;
#endif
        //          std::cerr << "function: " << fixedSize.first->curFunc->getNameStr() << "()" << std::endl;

        Checker::Result globalResult = globalCheckers.onCall(newState, *newState.getThread(tid), target, newState.curFunc, *newState.curArguments, NULL);
        Checker::Result localResult = newState.localCheckers.onCall(newState, *newState.getThread(tid), target, newState.curFunc, *newState.curArguments, NULL);
        if (UsePathSlicer)
            pathSlicer->recordCheckerResult((void *)&newState, globalResult, 
                    localResult, interpreterHandler->getNumTestCases());
        if (globalResult & Checker::ERROR || localResult & Checker::ERROR)
        {
            terminateStateOnError(newState, "checker reported error", "check.err");
            return true;
        }

        ref<Expr> ret = toUnique(newState, getDestCell(*newState.getThread(tid), target).value);
        globalResult = globalCheckers.onRet(newState, *newState.getThread(tid), target, ret);
        localResult = newState.localCheckers.onRet(newState, *newState.getThread(tid), target, ret);
        if (UsePathSlicer)
            pathSlicer->recordCheckerResult((void *)&newState, globalResult, 
                    localResult, interpreterHandler->getNumTestCases());
        if (globalResult & Checker::ERROR || localResult & Checker::ERROR)
        {
            terminateStateOnError(newState, "checker reported error", "check.err");
            return true;
        }
    }
    return false;
}

void Executor::executeFree(ExecutionState &state,
                           int tid,
                           ref<Expr> address,
                           KInstruction *target) {
  StatePair zeroPointer = fork(state, tid, Expr::createIsZero(address), true);
  if (zeroPointer.first) {
    if (target)
      bindLocal(target, *zeroPointer.first->getThread(tid), Expr::createPointer(0));
  }
  if (zeroPointer.second) { // address != 0
    ExactResolutionList rl;
    resolveExact(*zeroPointer.second, tid, address, rl, "free");
    
    for (Executor::ExactResolutionList::iterator it = rl.begin(), 
           ie = rl.end(); it != ie; ++it) {
      const MemoryObject *mo = it->first.first;
      if (mo->isLocal) {
        terminateStateOnError(*it->second, 
                              "free of alloca", 
                              "free.err",
                              getAddressInfo(*it->second, address));
      } else if (mo->isGlobal) {
        terminateStateOnError(*it->second, 
                              "free of global", 
                              "free.err",
                              getAddressInfo(*it->second, address));
      } else {
        it->second->addressSpace.unbindObject(mo);
        if (target)
          bindLocal(target, *it->second->getThread(tid), Expr::createPointer(0));
      }
    }
  }
}

void Executor::resolveExact(ExecutionState &state,
                            int tid,
                            ref<Expr> p,
                            ExactResolutionList &results, 
                            const std::string &name) {
  // XXX we may want to be capping this?
  ResolutionList rl;
  state.addressSpace.resolve(state, solver, p, rl);
  
  ExecutionState *unbound = &state;
  for (ResolutionList::iterator it = rl.begin(), ie = rl.end(); 
       it != ie; ++it) {
    ref<Expr> inBounds = EqExpr::create(p, it->first->getBaseExpr());
    
    StatePair branches = fork(*unbound, tid, inBounds, true);
    
    if (branches.first)
      results.push_back(std::make_pair(*it, branches.first));

    unbound = branches.second;
    if (!unbound) // Fork failure
      break;
  }

  if (unbound) {
    //printKLEECallStack(*unbound);
    terminateStateOnError(*unbound,
                          "memory error: invalid pointer: " + name,
                          "ptr.err",
                          getAddressInfo(*unbound, p));
  }
}

void Executor::executeMemoryOperation(ExecutionState &state,
                                      ThreadState &thr,
                                      bool isWrite,
                                      ref<Expr> address,
                                      ref<Expr> value /* undef if read */,
                                      KInstruction *target /* undef if write */) {
  Expr::Width type = (isWrite ? value->getWidth() : 
                     getWidthForLLVMType(target->inst->getType()));
  unsigned bytes = Expr::getMinBytesForWidth(type);

  if (SimplifySymIndices) {
    if (!isa<ConstantExpr>(address))
      address = state.constraints.simplifyExpr(address);
    if (isWrite && !isa<ConstantExpr>(value))
      value = state.constraints.simplifyExpr(value);
  }

  // fast path: single in-bounds resolution
  ObjectPair op;
  bool success;
  solver->setTimeout(stpTimeout);
  if (!state.addressSpace.resolveOne(state, solver, address, op, success)) {
    address = toConstant(state, address, "resolveOne failure");
    success = state.addressSpace.resolveOne(cast<ConstantExpr>(address), op);
  }
  solver->setTimeout(0);

  if (success) {
    const MemoryObject *mo = op.first;

    if (MaxSymArraySize && mo->size>=MaxSymArraySize) {
      address = toConstant(state, address, "max-sym-array-size");
    }
    
    ref<Expr> offset = mo->getOffsetExpr(address);

    bool inBounds;
    solver->setTimeout(stpTimeout);
    bool success = solver->mustBeTrue(state, 
                                      mo->getBoundsCheckOffset(offset, bytes),
                                      inBounds);
    solver->setTimeout(0);
    if (!success) {
      state.stepBack();
      terminateStateEarly(state, "query timed out");
      return;
    }

    if (inBounds) {
      const ObjectState *os = op.second;
      if (isWrite) {
        if (os->readOnly) {
          terminateStateOnError(state,
                                "memory error: object read only",
                                "readonly.err");
        } else {
          ObjectState *wos = state.addressSpace.getWriteable(mo, os);
          wos->write(offset, value);
        }          
      } else {
        ref<Expr> result = os->read(offset, type);
        
        if (interpreterOpts.MakeConcreteSymbolic)
          result = replaceReadWithSymbolic(state, result);
        
        bindLocal(target, thr, result);
      }

      return;
    }
  } 

  // we are on an error path (no resolution, multiple resolution, one
  // resolution with out of bounds)
  
  ResolutionList rl;  
  solver->setTimeout(stpTimeout);
  bool incomplete = state.addressSpace.resolve(state, solver, address, rl,
                                               0, stpTimeout);
  solver->setTimeout(0);
  
  // XXX there is some query wasteage here. who cares?
  ExecutionState *unbound = &state;
  
  for (ResolutionList::iterator i = rl.begin(), ie = rl.end(); i != ie; ++i) {
    const MemoryObject *mo = i->first;
    const ObjectState *os = i->second;
    ref<Expr> inBounds = mo->getBoundsCheckPointer(address, bytes);
    
    StatePair branches = fork(*unbound, thr.id, inBounds, true);
    ExecutionState *bound = branches.first;

    // bound can be 0 on failure or overlapped 
    if (bound) {
      if (isWrite) {
        if (os->readOnly) {
          terminateStateOnError(*bound,
                                "memory error: object read only",
                                "readonly.err");
        } else {
          ObjectState *wos = bound->addressSpace.getWriteable(mo, os);
          wos->write(mo->getOffsetExpr(address), value);
        }
      } else {
        ref<Expr> result = os->read(mo->getOffsetExpr(address), type);
        bindLocal(target, *bound->getThread(thr.id), result);
      }
    }

    unbound = branches.second;
    if (!unbound)
      break;
  }
  
  // XXX should we distinguish out of bounds and overlapped cases?
  if (unbound) {
    if (incomplete) {
      terminateStateEarly(*unbound, "query timed out (resolve)");
    } else {
      //printKLEECallStack(*unbound);
      terminateStateOnError(*unbound,
                            "memory error: out of bound pointer",
                            "ptr.err",
                            getAddressInfo(*unbound, address));
    }
  }
}

void Executor::executeMakeSymbolic(ExecutionState &state, 
                                   const MemoryObject *mo,
                                   const std::string &name) {
  // Create a new object state for the memory object (instead of a copy).
  if (!replayOut) {
    // Find a unique name for this array.  First try the original name,
    // or if that fails try adding a unique identifier.
    unsigned id = 0;
    std::string uniqueName = name;
    while (!state.arrayNames.insert(uniqueName).second) {
      uniqueName = name + "_" + llvm::utostr(++id);
    }
#ifdef MEMORY_DEBUG
    static int make_count = 0;
    make_count++;
    std::cerr << "*********** unique name: " << uniqueName << " count: " << make_count << std::endl;
#endif
    const Array *array = new Array(uniqueName, mo->size);
#ifdef MEMORY_DEBUG
    std::cerr << "mem usage before: " << sys::Process::GetTotalMemoryUsage() << "\n";
#endif
    bindObjectInState(state, NULL, mo, false, array);
    state.addSymbolic(mo, array);
#ifdef MEMORY_DEBUG
    std::cerr << "mem usage after: " << sys::Process::GetTotalMemoryUsage() << "\n";
#endif
    
    std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
      seedMap.find(&state);
    if (it!=seedMap.end()) { // In seed mode we need to add this as a
                             // binding.
      for (std::vector<SeedInfo>::iterator siit = it->second.begin(), 
             siie = it->second.end(); siit != siie; ++siit) {
        SeedInfo &si = *siit;
        KTestObject *obj = si.getNextInput(mo, NamedSeedMatching);

        if (!obj) {
          if (ZeroSeedExtension) {
            std::vector<unsigned char> &values = si.assignment.bindings[array];
            values = std::vector<unsigned char>(mo->size, '\0');
          } else if (!AllowSeedExtension) {
            terminateStateOnError(state, 
                                  "ran out of inputs during seeding",
                                  "user.err");
            break;
          }
        } else {
          if (obj->numBytes != mo->size &&
              ((!(AllowSeedExtension || ZeroSeedExtension)
                && obj->numBytes < mo->size) ||
               (!AllowSeedTruncation && obj->numBytes > mo->size))) {
	    std::stringstream msg;
	    msg << "replace size mismatch: "
		<< mo->name << "[" << mo->size << "]"
		<< " vs " << obj->name << "[" << obj->numBytes << "]"
		<< " in test\n";

            terminateStateOnError(state,
                                  msg.str(),
                                  "user.err");
            break;
          } else {
            std::vector<unsigned char> &values = si.assignment.bindings[array];
            values.insert(values.begin(), obj->bytes, 
                          obj->bytes + std::min(obj->numBytes, mo->size));
            if (ZeroSeedExtension) {
              for (unsigned i=obj->numBytes; i<mo->size; ++i)
                values.push_back('\0');
            }
          }
        }
      }
    }
  } else {
    ObjectState *os = bindObjectInState(state, NULL, mo, false);
    if (replayPosition >= replayOut->numObjects) {
      terminateStateOnError(state, "replay count mismatch", "user.err");
    } else {
      KTestObject *obj = &replayOut->objects[replayPosition++];
      if (obj->numBytes != mo->size) {
        terminateStateOnError(state, "replay size mismatch", "user.err");
      } else {
        for (unsigned i=0; i<mo->size; i++)
          os->write8(i, obj->bytes[i]);
      }
    }
  }
}

/***/

void Executor::runFunctionAsMain(Function *f,
				 int argc,
				 char **argv,
				 char **envp) {
  std::vector<ref<Expr> > arguments;

  // force deterministic initialization of memory objects
  srand(1);
  srandom(1);
  
  MemoryObject *argvMO = 0;

  // In order to make uclibc happy and be closer to what the system is
  // doing we lay out the environments at the end of the argv array
  // (both are terminated by a null). There is also a final terminating
  // null that uclibc seems to expect, possibly the ELF header?

  int envc;
  for (envc=0; envp[envc]; ++envc) ;

  unsigned NumPtrBytes = Context::get().getPointerWidth() / 8;
  KFunction *kf = kmodule->functionMap[f];
  assert(kf);
  Function::arg_iterator ai = f->arg_begin(), ae = f->arg_end();
  if (ai!=ae) {
    arguments.push_back(ConstantExpr::alloc(argc, Expr::Int32));

    if (++ai!=ae) {
      argvMO = memory->allocate((argc+1+envc+1+1) * NumPtrBytes, false, true,
                                f->begin()->begin());
      
      arguments.push_back(argvMO->getBaseExpr());

      if (++ai!=ae) {
        uint64_t envp_start = argvMO->address + (argc+1)*NumPtrBytes;
        arguments.push_back(Expr::createPointer(envp_start));

        if (++ai!=ae)
          klee_error("invalid main function (expect 0-3 arguments)");
      }
    }
  }

  ExecutionState *state = new ExecutionState(kmodule->functionMap[f]);
  
  if (pathWriter) 
    state->pathOS = pathWriter->open();
  if (symPathWriter) 
    state->symPathOS = symPathWriter->open();


  if (statsTracker)
    statsTracker->framePushed(*state, 0, 0);

  assert(arguments.size() == f->arg_size() && "wrong number of arguments");
  for (unsigned i = 0, e = f->arg_size(); i != e; ++i)
    bindArgument(kf, i, *state->getThread(0), arguments[i]);

  if (argvMO) {
    ObjectState *argvOS = bindObjectInState(*state, NULL, argvMO, false);

    for (int i=0; i<argc+1+envc+1+1; i++) {
      MemoryObject *arg;
      
      if (i==argc || i>=argc+1+envc) {
        arg = 0;
      } else {
        char *s = i<argc ? argv[i] : envp[i-(argc+1)];
        int j, len = strlen(s);
        
        arg = memory->allocate(len+1, false, true, state->lastThread()->pc->inst);
        ObjectState *os = bindObjectInState(*state, NULL, arg, false);
        for (j=0; j<len+1; j++)
          os->write8(j, s[j]);
      }

      if (arg) {
        argvOS->write(i * NumPtrBytes, arg->getBaseExpr());
      } else {
        argvOS->write(i * NumPtrBytes, Expr::createPointer(0));
      }
    }
  }
  
  initializeGlobals(*state);

  if (UseOneChecker == "") {
    globalCheckers.addChecker(new AssertChecker(this));
    state->localCheckers.addChecker(new OpenCloseChecker(this));
    state->localCheckers.addChecker(new LockChecker(this));
    state->localCheckers.addChecker(new FileChecker(this));
    state->localCheckers.addChecker(new DataLossChecker(this));
    state->localCheckers.addChecker(new SysOpenCloseChecker(this, true));
  } else {
    if (UseOneChecker == "Assert") {
      globalCheckers.addChecker(new AssertChecker(this));
    } else if (UseOneChecker == "OpenClose") {
      state->localCheckers.addChecker(new OpenCloseChecker(this));
    } else if (UseOneChecker == "Lock") {
      state->localCheckers.addChecker(new LockChecker(this));
    } else if (UseOneChecker == "File") {
      state->localCheckers.addChecker(new FileChecker(this));
    } else if (UseOneChecker == "DataLoss") {
      state->localCheckers.addChecker(new DataLossChecker(this));
    } else if (UseOneChecker == "Leak") {
      state->localCheckers.addChecker(new LeakChecker(this));
    } else if (UseOneChecker == "SOC") {
      state->localCheckers.addChecker(new SysOpenCloseChecker(this, true));
    } else if (UseOneChecker == "SOCR") {
      state->localCheckers.addChecker(new SysOpenCloseChecker(this, false));
    } else if (UseOneChecker == "None") {
      // NOP.
    } else
      assert(false && "UseOneChecker must be Assert, OpenClose, Lock, File, DataLoss, SOC(SysOpenClose), SOCR(SysOpenClose with Readonly) or None.");
  }
  
  processTree = new PTree(state);
  state->ptreeNode = processTree->root;
  run(*state);
  delete processTree;
  processTree = 0;

  globalCheckers.report(NULL);

  // hack to clear memory objects
  delete memory;
  memory = new MemoryManager();
  
  globalObjects.clear();
  globalAddresses.clear();

  if (statsTracker)
    statsTracker->done();

  if (UsePathSlicer) {
    pathSlicer->getKLEEFinalStat(stats::instructions.getValue(), stats::coveredInstructions.getValue(),
      stats::uncoveredInstructions.getValue(), interpreterHandler->getNumPathsExplored(),
      interpreterHandler->getNumTestCases(), !haltExecution);
    brTree.print();
  }
}

unsigned Executor::getPathStreamID(const ExecutionState &state) {
  assert(pathWriter);
  return state.pathOS.getID();
}

unsigned Executor::getSymbolicPathStreamID(const ExecutionState &state) {
  assert(symPathWriter);
  return state.symPathOS.getID();
}

void Executor::getConstraintLog(const ExecutionState &state,
                                std::string &res,
                                bool asCVC) {
  if (asCVC) {
    Query query(state.constraints, ConstantExpr::alloc(0, Expr::Bool));
    char *log = solver->stpSolver->getConstraintLog(query);
    res = std::string(log);
    free(log);
  } else {
    std::ostringstream info;
    ExprPPrinter::printConstraints(info, state.constraints);
    res = info.str();    
  }
}

bool Executor::getSymbolicSolution(const ExecutionState &state,
                                   std::vector< 
                                   std::pair<std::string,
                                   std::vector<unsigned char> > >
                                   &res) {
  solver->setTimeout(stpTimeout);

  ExecutionState tmp(state);
  if (!NoPreferCex) {
    for (unsigned i = 0; i != state.symbolics.size(); ++i) {
      const MemoryObject *mo = state.symbolics[i].first;
      std::vector< ref<Expr> >::const_iterator pi = 
        mo->cexPreferences.begin(), pie = mo->cexPreferences.end();
      for (; pi != pie; ++pi) {
        bool mustBeTrue;
        bool success = solver->mustBeTrue(tmp, Expr::createIsZero(*pi), 
                                          mustBeTrue);
        if (!success) break;
        if (!mustBeTrue) tmp.addConstraint(*pi);
      }
      if (pi!=pie) break;
    }
  }

  std::vector< std::vector<unsigned char> > values;
  std::vector<const Array*> objects;
  for (unsigned i = 0; i != state.symbolics.size(); ++i)
    objects.push_back(state.symbolics[i].second);
  bool success = solver->getInitialValues(tmp, objects, values);
  solver->setTimeout(0);
  if (!success) {
    klee_warning("unable to compute initial values (invalid constraints?)!");
    ExprPPrinter::printQuery(std::cerr,
                             state.constraints, 
                             ConstantExpr::alloc(0, Expr::Bool));
    return false;
  }
  
  for (unsigned i = 0; i != state.symbolics.size(); ++i)
    res.push_back(std::make_pair(state.symbolics[i].first->name, values[i]));
  return true;
}

void Executor::getCoveredLines(const ExecutionState &state,
                               std::map<const std::string*, std::set<unsigned> > &res) {
  res = state.coveredLines;
}

void Executor::doImpliedValueConcretization(ExecutionState &state,
                                            ref<Expr> e,
                                            ref<ConstantExpr> value) {
  abort(); // FIXME: Broken until we sort out how to do the write back.

  if (DebugCheckForImpliedValues)
    ImpliedValue::checkForImpliedValues(solver->solver, e, value);

  ImpliedValueList results;
  ImpliedValue::getImpliedValues(e, value, results);
  for (ImpliedValueList::iterator it = results.begin(), ie = results.end();
       it != ie; ++it) {
    ReadExpr *re = it->first.get();
    
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(re->index)) {
      // FIXME: This is the sole remaining usage of the Array object
      // variable. Kill me.
      const MemoryObject *mo = 0; //re->updates.root->object;
      const ObjectState *os = state.addressSpace.findObject(mo);

      if (!os) {
        // object has been free'd, no need to concretize (although as
        // in other cases we would like to concretize the outstanding
        // reads, but we have no facility for that yet)
      } else {
        assert(!os->readOnly && 
               "not possible? read only object with static read?");
        ObjectState *wos = state.addressSpace.getWriteable(mo, os);
        wos->write(CE, it->second);
      }
    }
  }
}

Expr::Width Executor::getWidthForLLVMType(LLVM_TYPE_Q llvm::Type *type) const {
  return kmodule->targetData->getTypeSizeInBits(type);
}

bool Executor::curInstrIsInternal(KInstruction *kInstr) {
  bool result = pathSlicer->isInternalInstr(kInstr->inst);
  return result;
}

void Executor::slicingAndPruneStates(ExecutionState &state) {
  std::set<size_t> rmBrs;
  std::set<size_t> rmCalls;
  pathSlicer->runPathSlicer((void *)&state, rmBrs, rmCalls, haltExecution);  // This must be run by all states, collect states stat.
  if (!state.isPruned) {
    std::set<ExecutionState *> prunedStates;
    std::set<size_t>::iterator itr;
    for (itr = rmBrs.begin(); itr != rmBrs.end(); itr++) {
      brTree.findPrunedStates(&state, *itr,  prunedStates);
    }
    for (itr = rmCalls.begin(); itr != rmCalls.end(); itr++) {
      brTree.findPrunedStates(&state, *itr,  prunedStates);
    }
  
    // Prune.
    std::set<ExecutionState *>::iterator stateItr;
    for (stateItr = prunedStates.begin(); stateItr != prunedStates.end(); stateItr++) {
      ExecutionState *prunedState = *stateItr;
      prunedState->isPruned = true;
      if (!MarkPrunedOnly)
        terminateStateAtOnce(*prunedState);
    }
  }
}

void Executor::printKLEECallStack(ExecutionState &state) {
  ThreadState &thr = (*state.threads.find(0)).second; // Only prints the main thread for now.
  Instruction *curInstr = thr.prevPC->inst;
  BasicBlock *BB = curInstr->getParent();    
  errs() << "Executor::printKLEECallStack Inst " << *curInstr 
    << ", BB " << BB->getNameStr()
    << ", F " << BB->getParent()->getNameStr() << "\n";
  for (unsigned i = 0; i < thr.stack.size(); i++) {
    StackFrame &sf = thr.stack.at(i);
    assert(sf.kf->function->hasName());
    fprintf(stderr, "%s, ", sf.kf->function->getNameStr().c_str());
	KInstIterator kcaller = sf.caller;
    Instruction *instr = kcaller?kcaller->inst:NULL;
    if (instr)
      errs() << "caller: " << *instr;
    else
      errs() << "caller: unknown";
    fprintf(stderr, "\n");
  }
  fprintf(stderr, "\n\n\n");
}

void Executor::terminateStateAtOnce(ExecutionState &state) {
  std::set<size_t> rmBrs;
  std::set<size_t> rmCalls;
  pathSlicer->collectStatesStat((void *)&state);
  
  // searcher.
  if (searcher) {
    std::set< ExecutionState*> added;
    std::set< ExecutionState*> removed;
    removed.insert(&state);
    searcher->update(0, added, removed);
  }

  //addedStates
  addedStates.erase(&state);

  // removedStates
  removedStates.erase(&state);

  // states
  states.erase(&state);

  // seedMap.
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it3 = 
    seedMap.find(&state);
  if (it3 != seedMap.end())
    seedMap.erase(it3);

  // processTree.
  processTree->remove(state.ptreeNode);

  // Clear path slicer.
  if (UsePathSlicer) {
    pathSlicer->clearPath((void *)&state);
    brTree.removeState(&state);
  }

  //FIXME: SHOULD DELETE HERE, BUT THERE WAS A MEM ADDR CRASH.
  //delete &state;
}


///

Interpreter *Interpreter::create(const InterpreterOptions &opts,
                                 InterpreterHandler *ih) {
  return new Executor(opts, ih);
}
