#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/PassManager.h"
#include "llvm/ADT/ilist.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Type.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/DebugInfo.h"
#include "llvm/Analysis/ProfileInfoLoader.h"
#include "Transforms/Instrumentation/ProfilingUtils.h"
#include "llvm/Analysis/ProfileInfoLoader.h"
#include "llvm/Analysis/ProfileInfo.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/InstIterator.h"

#include <iostream>
#include <vector>

using namespace llvm;
using namespace std;

namespace soaap {
  struct DOTDynCG : public ModulePass {

    public:
      static char ID;
      static Instruction* DynamicInstruction;

      DOTDynCG() : ModulePass(ID) { }

      virtual void getAnalysisUsage(AnalysisUsage &AU) const;

      virtual bool runOnModule(Module& M);

    private:
      CallInst* constructDummyCallInst(Module& M);
  };
}

namespace llvm {
  template<>
  struct DOTGraphTraits<CallGraph*> : public DefaultDOTGraphTraits {

    DOTGraphTraits (bool isSimple=false) : DefaultDOTGraphTraits(isSimple) {}

    static bool isStrict(CallGraph *G) {
      return true;
    }

    static std::string getGraphName(CallGraph *G) {
      return "Dynamic Call Graph";
    }

    static std::string getNodeLabel(CallGraphNode *Node, CallGraph *Graph) {
      if (Node->getFunction())
        return ((Value*)Node->getFunction())->getName();
      return "external node";
    }   

    // A node is hidden if it has no outgoing or incoming dynamic edges. For the purposes
    // of this LLVM pass, we are concerned with finding dynamic edges for which there is no
    // equivalent static edge. This helps identify incompleteness in the static call graph.
    static std::string getNodeAttributes(CallGraphNode* N, CallGraph* CG) {
      for (CallGraph::iterator CI = CG->begin(), CE = CG->end(); CI != CE; CI++) {
        CallGraphNode* T1 = CI->second;
        for (CallGraphNode::iterator NI = T1->begin(), NE = T1->end(); NI != NE; NI++) {
          Value* V = NI->first;
          CallGraphNode *T2 = NI->second;
          if (V == soaap::DOTDynCG::DynamicInstruction && (T1 == N || T2 == N)) 
            return ""; 
        }   
      }        
      return "style=invis";
    }   

    static std::string getEdgeAttributes(CallGraphNode *CallerNode, GraphTraits<CallGraph*>::ChildIteratorType edgeIt, CallGraph *Graph) {
      CallGraphNode* CalleeNode = edgeIt.getCurrent()->second;
      // iterate through edges from CallerNode and determine for CallerNode -> CalleeNode
      // if either a) dynamic and static edges exist or b) just dynamic or c) just static
      bool dynamicEdge = false;
      bool staticEdge = false;
      for (CallGraphNode::iterator I = CallerNode->begin(), E = CallerNode->end(); I != E; I++) {
        Value* V = I->first;
        CallGraphNode* T = I->second;
        if (T == CalleeNode) {
          if (V == soaap::DOTDynCG::DynamicInstruction)
            dynamicEdge = true;
          else if (V) 
            staticEdge = true;
        }   
      }   
      // Based on a), b) or c) we set the edge attributes.
      // static-only edges are made invisible as they are irrelevant
      if (dynamicEdge && staticEdge)
        return "color=\"green\"";
      else if (dynamicEdge)
        return "color=\"red\"";
      else /* covers static edges and edges to/from external node */
        return "style=invis";
    }

   };
}

