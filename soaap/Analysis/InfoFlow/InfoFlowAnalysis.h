#ifndef SOAAP_ANALYSIS_INFOFLOW_INFOFLOWANALYSIS_H
#define SOAAP_ANALYSIS_INFOFLOW_INFOFLOWANALYSIS_H

#include <map>
#include <list>

#include "llvm/DebugInfo.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Pass.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/Local.h"

#include "ADT/QueueSet.h"
#include "Analysis/Analysis.h"
#include "Analysis/InfoFlow/InfoFlowAnalysis.h"
#include "Common/Debug.h"
#include "Util/CallGraphUtils.h"
#include "Util/ContextUtils.h"
#include "Util/DebugUtils.h"
#include "Util/LLVMAnalyses.h"
#include "Util/SandboxUtils.h"

using namespace std;
using namespace llvm;

namespace soaap {
  // Base class for context-sensitive information-flow analysis.
  // There are three types of context: no context, privileged context and sandbox.
  // A fourth context "single" is used for context-insensitivity
  // These contexts are found in Context.h
  template<class FactType>
  class InfoFlowAnalysis : public Analysis {
    public:
      typedef map<const Value*, FactType> DataflowFacts;
      typedef pair<const Value*, Context*> ValueContextPair;
      typedef QueueSet<ValueContextPair> ValueContextPairList;
      InfoFlowAnalysis(bool c = false, bool m = false) : contextInsensitive(c), mustAnalysis(m) { }
      virtual void doAnalysis(Module& M, SandboxVector& sandboxes);

    protected:
      map<Context*, DataflowFacts> state;
      bool contextInsensitive;
      bool mustAnalysis;
      virtual void initialise(ValueContextPairList& worklist, Module& M, SandboxVector& sandboxes) = 0;
      virtual void performDataFlowAnalysis(ValueContextPairList&, SandboxVector& sandboxes, Module& M);
      // performMeet: toVal = fromVal /\ toVal. return true <-> toVal != fromVal /\ toVal
      virtual bool performMeet(FactType fromVal, FactType& toVal) = 0;
      virtual bool propagateToValue(const Value* from, const Value* to, Context* cFrom, Context* cTo, Module& M);
      virtual void propagateToCallees(CallInst* CI, const Value* V, Context* C, bool propagateAllArgs, ValueContextPairList& worklist, SandboxVector& sandboxes, Module& M);
      virtual void propagateToCallers(ReturnInst* RI, const Value* V, Context* C, ValueContextPairList& worklist, SandboxVector& sandboxes, Module& M);
      virtual Value* propagateForExternCall(CallInst* CI, const Value* V);
      virtual void postDataFlowAnalysis(Module& M, SandboxVector& sandboxes) = 0;
      virtual void addToWorklist(const Value* V, Context* C, ValueContextPairList& worklist);
      virtual FactType bottomValue() = 0;
      virtual string stringifyFact(FactType f) = 0;
      virtual string stringifyValue(const Value* V);
      virtual void stateChangedForFunctionPointer(CallInst* CI, const Value* FP, FactType& newState);
      virtual void propagateToAggregate(const Value* V, Context* C, Value* Agg, ValueSet& visited, ValueContextPairList& worklist, SandboxVector& sandboxes, Module& M);
  };

  template <class FactType>
  void InfoFlowAnalysis<FactType>::doAnalysis(Module& M, SandboxVector& sandboxes) {
    ValueContextPairList worklist;
    initialise(worklist, M, sandboxes);
    performDataFlowAnalysis(worklist, sandboxes, M);
    postDataFlowAnalysis(M, sandboxes);
  }

  template <typename FactType>
  void InfoFlowAnalysis<FactType>::performDataFlowAnalysis(ValueContextPairList& worklist, SandboxVector& sandboxes, Module& M) {

    // merge contexts if this is a context-insensitive analysis
    if (contextInsensitive) {
      SDEBUG(debugs("soaap.infoflow",3) << INDENT_1 << "Merging contexts\n")
      worklist.clear();
      for (typename map<Context*,DataflowFacts>::iterator I=state.begin(), E=state.end(); I != E; I++) {
        Context* C = I->first;
        DataflowFacts F = I->second;
        for (typename DataflowFacts::iterator DI=F.begin(), DE=F.end(); DI != DE; DI++) {
          const Value* V = DI->first;
          FactType T = DI->second;
          state[ContextUtils::SINGLE_CONTEXT][V] = T; // TODO: what if same V appears in multiple contexts?
          addToWorklist(V, ContextUtils::SINGLE_CONTEXT, worklist);
        }
      }
    }

    // perform propagation until fixed point is reached
    while (!worklist.empty()) {
      ValueContextPair P = worklist.dequeue();
      //ValueContextPair P = worklist.pop_back_val();
      const Value* V = P.first;
      Context* C = P.second;
      //worklist.pop_front();

      SDEBUG(debugs("soaap.infoflow",3) << INDENT_1 << "Popped " << V->getName() << ", context: "
              << ContextUtils::stringifyContext(C) << ", value dump: " << stringifyValue(V) << "\n"
              << "state[C][V]: " << stringifyFact(state[C][V]) << "\n" 
              << INDENT_2 << "Finding uses (" << V->getNumUses() << ")\n")
      for (Value::const_use_iterator UI=V->use_begin(), UE=V->use_end(); UI != UE; UI++) {
        User* U = dyn_cast<User>(UI.getUse().getUser());
        SDEBUG(debugs("soaap.infoflow",4) << INDENT_3 << "Use: " << stringifyValue(U) << "\n")
        const Value* V2 = NULL;
        if (Constant* CS = dyn_cast<Constant>(U)) {
          V2 = CS;
          if (propagateToValue(V, V2, C, C, M)) { // propagate taint from (V,C) to (V2,C)
            SDEBUG(debugs("soaap.infoflow",3) << INDENT_4 << "Propagating (" << stringifyValue(V)
                    << ", " << ContextUtils::stringifyContext(C) << ") to (" << stringifyValue(V2) << "\n"
                    << ", " << ContextUtils::stringifyContext(C) << ")\n");
            addToWorklist(V2, C, worklist);
          }
        }
        else if (Instruction* I = dyn_cast<Instruction>(UI.getUse().getUser())) {
          if (C == ContextUtils::NO_CONTEXT) {
            // update the taint value for the correct context and put the new pair on the worklist
            ContextVector C2s = ContextUtils::getContextsForMethod(I->getParent()->getParent(), contextInsensitive, sandboxes, M);
            for (Context* C2 : C2s) {
            SDEBUG(debugs("soaap.infoflow",3) << INDENT_4 << "Propagating (" << stringifyValue(V)
                    << ", " << ContextUtils::stringifyContext(C) << ") to (" << stringifyValue(V) << "\n"
                    << ", " << ContextUtils::stringifyContext(C2) << ")\n");
              propagateToValue(V, V, C, C2, M); 
              addToWorklist(V, C2, worklist);
            }
          }
          else if (ContextUtils::isInContext(I, C, contextInsensitive, sandboxes, M)) { // check if using instruction is in context C
            if (StoreInst* SI = dyn_cast<StoreInst>(I)) {
              if (V == SI->getPointerOperand()) // to avoid infinite looping
                continue;
              V2 = SI->getPointerOperand();
            }
            else if (IntrinsicInst* II = dyn_cast<IntrinsicInst>(I)) {
              if (II->getIntrinsicID() == Intrinsic::ptr_annotation) { // covers llvm.ptr.annotation.p0i8
                SDEBUG(II->dump());
                V2 = II;
              }
            }
            else if (CallInst* CI = dyn_cast<CallInst>(I)) {
              // propagate to the callee(s)
              SDEBUG(debugs("soaap.infoflow",3) << INDENT_4 << "Call instruction; propagating to callees\n");
              if (CallGraphUtils::isExternCall(CI)) { // no function body, so we approximate effects of known funcs
                V2 = propagateForExternCall(CI, V);
              }
              else {
                bool propagateAllArgs = false;
                if (CI->getCalledValue() == V) {
                  // subclasses might want to be informed when
                  // the state of a function pointer changed
                  stateChangedForFunctionPointer(CI, V, state[C][V]);
                  
                  // if callee information has changed, we should propagate all
                  // args to callees in case this is the first time for some
                  propagateAllArgs = true;
                }
                propagateToCallees(CI, V, C, propagateAllArgs, worklist, sandboxes, M);
                continue;
              }
            }
            else if (ReturnInst* RI = dyn_cast<ReturnInst>(I)) {
              if (Value* RetVal = RI->getReturnValue()) {
                SDEBUG(debugs("soaap.infoflow",3) << INDENT_4 << "Return instruction; propagating to callers\n");
                propagateToCallers(RI, RetVal, C, worklist, sandboxes, M);
              }
              continue;
            }
            else if (I->isBinaryOp()) {
              // The resulting value is a combination of its operands and we do not combine
              // dataflow facts in this way. So we do not propagate the dataflow-value of V
              // but actually set it to the bottom value.
              SDEBUG(debugs("soaap.infoflow",4) << INDENT_4 << "Binary operator, propagating bottom to " << *I << "\n");
              state[C][I] = bottomValue();
              addToWorklist(I, C, worklist);
              continue;
            }
            else if (PHINode* PHI = dyn_cast<PHINode>(I)) {
              // take the meet of all incoming values
              bool change = false;
              int i;
              for (i=0; i<PHI->getNumIncomingValues(); i++) {
                Value* IV = PHI->getIncomingValue(i);
                if (propagateToValue(IV, PHI, C, C, M)) {
                  change = true;
                }
              }
              if (change) {
                addToWorklist(PHI, C, worklist);
              }
            }
            else {
              //debugs() << "Unaccounted-for instruction: " << *I << "\n";
              V2 = I; // this covers gep instructions
            }
            if (V2 != NULL && propagateToValue(V, V2, C, C, M)) { // propagate taint from (V,C) to (V2,C)
              SDEBUG(debugs("soaap.infoflow",3) << INDENT_4 << "Propagating (" << stringifyValue(V)
                       << ", " << ContextUtils::stringifyContext(C) << ") to (" << stringifyValue(V2)
                       << ", " << ContextUtils::stringifyContext(C) << ")\n");
              addToWorklist(V2, C, worklist);

              // special case for GEP (propagate to the aggregate, if we stored to it)
              if (isa<StoreInst>(U) && isa<GetElementPtrInst>(V2)) {
                SDEBUG(debugs("soaap.infoflow",3) << INDENT_2 << "storing to GEP\n");
                // rewind to the aggregate and propagate
                ValueSet visited;
                propagateToAggregate(V2, C, (Value*)V2, visited, worklist, sandboxes, M);
              }
              else if (CallInst* CI = dyn_cast<CallInst>(I)) {
                if (CallGraphUtils::isExternCall(CI) && V2 != CI) {
                  // propagating to one of the args, and not the return value. we propagate back
                  // as much as possible
                  ValueSet visited;
                  propagateToAggregate(V2, C, (Value*)V2, visited, worklist, sandboxes, M);
                }
              }
            }
          }
        }
      }
    }

    // unmerge contexts if this is a context-insensitive analysis
    if (contextInsensitive) {
      SDEBUG(debugs("soaap.infoflow",3) << INDENT_1 << "Unmerging contexts\n");
      ContextVector Cs = ContextUtils::getAllContexts(sandboxes);
      DataflowFacts F = state[ContextUtils::SINGLE_CONTEXT];
      for (typename DataflowFacts::iterator I=F.begin(), E=F.end(); I != E; I++) {
        const Value* V = I->first;
        FactType T = I->second;
        for (Context* C : Cs) {
          state[C][V] = T;
        }
      }
    }

  }

  template<typename FactType>
  void InfoFlowAnalysis<FactType>::propagateToAggregate(const Value* V, Context* C, Value* Agg, ValueSet& visited, ValueContextPairList& worklist, SandboxVector& sandboxes, Module& M) {
    SDEBUG(debugs("soaap.infoflow",4) << "Agg: " << *Agg << "\n");
    Agg = Agg->stripInBoundsOffsets();
    if (visited.count(Agg) == 0) {
      visited.insert(Agg);
      if (isa<AllocaInst>(Agg) || isa<GlobalVariable>(Agg) || isa<Argument>(Agg)) {
        if (propagateToValue(V, Agg, C, C, M)) { 
          SDEBUG(debugs("soaap.infoflow",3) << INDENT_3 << "propagating to aggregate\n");
          addToWorklist(Agg, C, worklist);
        }
      }
      else {
        if (GetElementPtrInst* gep = dyn_cast<GetElementPtrInst>(Agg)) {
          propagateToAggregate(V, C, gep->getPointerOperand(), visited, worklist, sandboxes, M);
        }
        else if (LoadInst* load = dyn_cast<LoadInst>(Agg)) {
          propagateToAggregate(V, C, load->getPointerOperand(), visited, worklist, sandboxes, M);
        }
        else if (IntrinsicInst* intrins = dyn_cast<IntrinsicInst>(Agg)) {
          if (intrins->getIntrinsicID() == Intrinsic::ptr_annotation) { // covers llvm.ptr.annotation.p0i8
            propagateToAggregate(V, C, intrins->getArgOperand(0), visited, worklist, sandboxes, M);
          }
          else {
            SDEBUG(debugs("soaap.infoflow",4) << "WARNING: unexpected intrinsic instruction: " << *Agg << "\n");
          }
        }
        else if (CallInst* call = dyn_cast<CallInst>(Agg)) {
          // in this case, we need to rewind back to the local/global Value*
          // that flows to the return value of call's callee. This would
          // involve doing more information flow analysis. However, for now
          // we hardcode the specific cases:
          //TODO: replace!
          if (Function* F = call->getCalledFunction()) {
            if (F->getName() == "buffer_ptr") {
              propagateToAggregate(V, C, call->getArgOperand(0), visited, worklist, sandboxes, M);
            }
          }
          else {
            SDEBUG(debugs("soaap.infoflow",4) << "WARNING: unexpected call instruction: " << *Agg << "\n");
          }
        }
        else if (SelectInst* select = dyn_cast<SelectInst>(Agg)) {
          propagateToAggregate(V, C, select->getTrueValue(), visited, worklist, sandboxes, M);
          propagateToAggregate(V, C, select->getFalseValue(), visited, worklist, sandboxes, M);
        }
        else if (PHINode* phi = dyn_cast<PHINode>(Agg)) {
          for (int i=0; i<phi->getNumIncomingValues(); i++) {
            propagateToAggregate(V, C, phi->getIncomingValue(i), visited, worklist, sandboxes, M);
          }
        }
        else {
          SDEBUG(debugs("soaap.infoflow",4) << "WARNING: unexpected value: " << *Agg << "\n"
                    << "Value type: " << Agg->getValueID() << "\n");
        }
        // propagate to this Value*
        if (propagateToValue(V, Agg, C, C, M)) { 
          addToWorklist(Agg, C, worklist);
        }
      }
      
      // if Agg is a struct parameter, then it probably outlives this function and
      // so we should propagate the targets of function pointers it contains to the 
      // calling context (i.e. to the corresponding caller's arg value)
      //TODO: how do we know A is specifically a struct parameter?
      Argument* A = dyn_cast<Argument>(Agg);
      Function* enclosingFunc = (A == NULL) ? NULL : A->getParent();
      if (A == NULL) { // if Agg is an AllocaInst then obtain the corresponding 
        if (AllocaInst* AI = dyn_cast<AllocaInst>(Agg)) {
          if (DbgDeclareInst* dbgDecl = FindAllocaDbgDeclare(AI)) {
            enclosingFunc = AI->getParent()->getParent();
            DIVariable varDbg(dbgDecl->getVariable());
            int argNum = varDbg.getArgNumber(); // arg nums start from 1
            if (argNum > 0) {
              // Agg is a param 
              string argName = varDbg.getName().str();
              for (Argument &arg : enclosingFunc->getArgumentList()) {
                if (arg.getName().str() == argName) {
                  A = &arg;
                  break;
                }
              }
            }
          }
        }
      }
      if (A != NULL) {
        // we found the param index, propagate back to all caller args
        int argIdx = A->getArgNo();
        for (CallInst* caller : CallGraphUtils::getCallers(enclosingFunc, M)) {
          Function* callerFunc = caller->getParent()->getParent();
          ContextVector callerContexts = ContextUtils::getContextsForMethod(callerFunc, contextInsensitive, sandboxes, M);
          Value* arg = caller->getArgOperand(argIdx);
          //debugs() << "Propagating arg " << *A << " to caller " << callerFunc->getName() << "\n";
          SDEBUG(debugs("soaap.infoflow",4) << INDENT_2 << "Adding arg " << *arg << " to worklist\n");
          for (Context* C2 : callerContexts) {
            if (propagateToValue(V, arg, C, C2, M)) {
              addToWorklist(arg, C2, worklist);
            }
            propagateToAggregate(arg, C2, arg, visited, worklist, sandboxes, M);
          }
        }
      }
    }
  }

  template <typename FactType>
  void InfoFlowAnalysis<FactType>::addToWorklist(const Value* V, Context* C, ValueContextPairList& worklist) {
    ValueContextPair P = make_pair(V, C);
    worklist.enqueue(P);
  }

  template <typename FactType>
  bool InfoFlowAnalysis<FactType>::propagateToValue(const Value* from, const Value* to, Context* cFrom, Context* cTo, Module& M) {
    if (state[cTo].find(to) == state[cTo].end()) {
      state[cTo][to] = state[cFrom][from];
      return true; // return true to allow state to propagate through
                   // regardless of whether the value was non-bottom
    }                   
    else {
      //FactType old = state[cTo][to];
      //state[cTo][to] = performMeet(state[cFrom][from], old);
      return performMeet(state[cFrom][from], state[cTo][to]);
    }
  }


  template <typename FactType>
  void InfoFlowAnalysis<FactType>::propagateToCallees(CallInst* CI, const Value* V, Context* C, bool propagateAllArgs, ValueContextPairList& worklist, SandboxVector& sandboxes, Module& M) {

    SDEBUG(debugs("soaap.infoflow",4) << "Call instruction: " << *CI << "\n"
              << "Calling-context C: " << ContextUtils::stringifyContext(C) << "\n");

    FunctionSet callees = CallGraphUtils::getCallees(CI, M);
    SDEBUG(debugs("soaap.infoflow",4) << INDENT_5 << "callees: " << CallGraphUtils::stringifyFunctionSet(callees) << "\n");

    for (int argIdx=0; argIdx<CI->getNumArgOperands(); argIdx++) {
      if (propagateAllArgs || CI->getArgOperand(argIdx) == V) {
        const Value* V2 = NULL;
        for (Function* callee : callees) {
          Context* C2 = ContextUtils::calleeContext(C, contextInsensitive, callee, sandboxes, M);
          SDEBUG(debugs("soaap.infoflow",4) << INDENT_5 << "Propagating to callee " << callee->getName() << "\n"
                    << INDENT_6 << "Callee-context C2: " << ContextUtils::stringifyContext(C2) << "\n");
          Function::ArgumentListType& params = callee->getArgumentList();
          
          // check if value being propagated is a var arg, and propagate
          // accordingly
          if (argIdx >= callee->getFunctionType()->getNumParams()) { // var arg
            BasicBlock& EntryBB = callee->getEntryBlock();
            
            // find va_list var, it will have type [1 x %struct.__va_list_tag]*
            for (BasicBlock::iterator I = EntryBB.begin(), E = EntryBB.end(); I != E; I++) {
              if (AllocaInst* Alloca = dyn_cast<AllocaInst>(I)) {
                if (ArrayType* AT = dyn_cast<ArrayType>(Alloca->getAllocatedType())) {
                  if (StructType* ST = dyn_cast<StructType>(AT->getElementType())) {
                    SDEBUG(debugs("soaap.infoflow",4) << "Struct type has name: " << ST->getName() << "\n");
                    if (ST->getName() == "struct.__va_list_tag") {
                      V2 = Alloca;
                      break;
                    }
                  }
                }
              }
            }
          }
          else {
            // NOTE: no way to index a function's list of parameters
            Function::const_arg_iterator AI = callee->arg_begin();
            int i = 0;
            while (i++ < argIdx) { AI++; }
            V2 = AI;
          }

          if (V2 != NULL) {
            SDEBUG(debugs("soaap.infoflow",4) << INDENT_6 << "Propagating to " << stringifyValue(V2));
            
            // if this is a must analysis, take meet of all argument values
            // passed in at argIdx by all callers in context C. This makes our 
            // analysis sound when our meet operator is intersection
            bool change = false;
            if (mustAnalysis) {
              // To be sound, we need to take the meet of all values passed in for each
              // parameter that we are propagating to (i.e. from all other call sites and
              // not only CI). Otherwise, must analyses will lead to incorrect results.
              CallInstSet callers = CallGraphUtils::getCallers(callee, M);
              SDEBUG(debugs("soaap.infoflow",4) << INDENT_6 << "Taking meet of all arg idx " << argIdx << " values from all callers\n");
              for (CallInst* caller : callers) { // CI will be in callers
                if (ContextUtils::isInContext(caller, C, contextInsensitive, sandboxes, M)) {
                  SDEBUG(debugs("soaap.infoflow",4) << INDENT_6 << "Caller: " << *caller << " (enclosing func: " << caller->getParent()->getParent()->getName() << ")\n");
                  Value* V3 = caller->getArgOperand(argIdx);
                  if (propagateToValue(V3, V2, C, C2, M)) { // propagate
                    change = true;
                  }
                }
              }
            }
            else {
              change = propagateToValue(V, V2, C, C2, M);
            }
            if (change) {
              SDEBUG(debugs("soaap.infoflow",4) << "Adding (V2,C2) to worklist\n"
                        << "state[C2][V2]: " << stringifyFact(state[C2][V2]) << "\n");
              addToWorklist(V2, C2, worklist);
            }
          }
        }
      }
    }
  }

  template <typename FactType>
  void InfoFlowAnalysis<FactType>::propagateToCallers(ReturnInst* RI, const Value* V, Context* C, ValueContextPairList& worklist, SandboxVector& sandboxes, Module& M) {
    Function* F = RI->getParent()->getParent();
    for (CallInst* CI : CallGraphUtils::getCallers(F, M)) {
      SDEBUG(debugs("soaap.infoflow",4) << INDENT_5 << "Propagating to caller " << stringifyValue(CI));
      ContextVector C2s = ContextUtils::callerContexts(RI, CI, C, contextInsensitive, sandboxes, M);
      for (Context* C2 : C2s) {
        if (propagateToValue(V, CI, C, C2, M)) {
          addToWorklist(CI, C2, worklist);
        }
      }
    }
  }

  template <typename FactType>
  Value* InfoFlowAnalysis<FactType>::propagateForExternCall(CallInst* CI, const Value* V) {
    // propagate dataflow value of relevant arg(s) (if happen to be V) to
    // the return value of the call CI
    if (Function* F = CallGraphUtils::getDirectCallee(CI)) {
      SDEBUG(debugs("soaap.infoflow",4) << "Extern call, f=" << F->getName() << "\n");
      string funcName = F->getName();
      if (funcName == "strdup") {
        return CI;
      }
      else if (funcName == "asprintf" || funcName == "vasprintf") { 
        // if V is not the format string or the out param, then propagate to out param
        if (CI->getArgOperand(0) != V && CI->getArgOperand(1) != V) {
          return CI->getArgOperand(0);
        }
      }
      else if (funcName == "strcpy") {
        if (CI->getArgOperand(0) != V) { // V is not dst, so it must be src
          return CI->getArgOperand(0);
        }
      }
      else if (funcName == "g_ptr_array_add") {
        if (CI->getArgOperand(1) == V) {
          return CI->getArgOperand(0);
        }
      }
      else if (funcName == "g_hash_table_insert") {
        if (CI->getArgOperand(2) == V) {
          SDEBUG(debugs("soaap.infoflow",4) << "g_hash_table_insert and 2nd arg op\n"
                  << *CI << "\n"
                  << "fact for V: " << stringifyFact(state[ContextUtils::SINGLE_CONTEXT][V]) << "\n");
          return CI->getArgOperand(0);
        }
      }
      else if (funcName == "g_hash_table_lookup") {
        if (CI->getArgOperand(0) == V) {
          SDEBUG(debugs("soaap.infoflow",4) << "g_hash_table_lookup and first arg op\n"
                   << "fact for V: " << stringifyFact(state[ContextUtils::SINGLE_CONTEXT][V]) << "\n");
          return CI;
        }
      }
      else {
        static FunctionSet unknownExterns;
        if (unknownExterns.count(F) == 0) {
          unknownExterns.insert(F);
          SDEBUG(debugs("soaap.infoflow",4) << "SOAAP ERROR: Propagation has reached unknown extern function call to " << funcName << "\n");
        }
      }
    }
    SDEBUG(debugs("soaap.infoflow",4) << "Returning NULL\n");
    return NULL;
  }

  template <typename FactType>
  string InfoFlowAnalysis<FactType>::stringifyValue(const Value* V) {
    string result;
    raw_string_ostream ss(result);
    if (isa<Function>(V)) {
      ss << V->getName();
    }
    else {
      V->print(ss);
    }
    ss << " - " << *V->getType();
    return result;
  }

  // default behaviour is to do nothing
  template<typename FactType>
  void InfoFlowAnalysis<FactType>::stateChangedForFunctionPointer(CallInst* CI, const Value* FP, FactType& newState) {
  }
}

#endif
