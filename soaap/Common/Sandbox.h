#ifndef SOAAP_COMMON_SANDBOX_H
#define SOAAP_COMMON_SANDBOX_H

#include "Analysis/InfoFlow/Context.h"
#include "Common/Typedefs.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Analysis/CallGraph.h"

using namespace llvm;

namespace soaap {
  typedef map<GlobalVariable*,int> GlobalVariableIntMap;
  class Sandbox : public Context {
    public:
      Sandbox(string n, int i, Function* entry, bool p, Module& m, int o, int c);
      Sandbox(string n, int i, InstVector& region, bool p, Module& m);
      string getName();
      int getNameIdx();
      Function* getEntryPoint();
      FunctionVector getFunctions();
      GlobalVariableIntMap getGlobalVarPerms();
      ValueIntMap getCapabilities();
      bool isAllowedToReadGlobalVar(GlobalVariable* gv);
      FunctionVector getCallgates();
      bool isCallgate(Function* F);
      int getClearances();
      int getOverhead();
      bool isPersistent();
      CallInstVector getCreationPoints();
      bool containsFunction(Function* F);
      bool hasCallgate(Function* F);
      static bool classof(const Context* C) { return C->getKind() == CK_SANDBOX; }

    private:
      Module& module;
      string name;
      int nameIdx;
      Function* entryPoint;
      InstVector region;
      bool persistent;
      int clearances;
      FunctionVector callgates;
      FunctionVector functionsVec;
      DenseSet<Function*> functionsSet;
      CallInstVector creationPoints;
      GlobalVariableIntMap sharedVarToPerms;
      ValueIntMap caps;
      int overhead;
      
      void findSandboxedFunctions();
      void findSandboxedFunctionsHelper(CallGraphNode* n);
      void findSharedGlobalVariables();
      void findCallgates();
      void findCapabilities();
      void findCreationPoints();
      void validateEntryPointCalls();
      bool validateEntryPointCallsHelper(BasicBlock* BB, BasicBlockVector& visited, InstTrace& trace);
  };
  typedef SmallVector<Sandbox*,16> SandboxVector;
}

#endif