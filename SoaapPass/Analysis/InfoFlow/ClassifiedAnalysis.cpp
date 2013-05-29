#include "Analysis/InfoFlow/ClassifiedAnalysis.h"
#include "Utils/ClassifiedUtils.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/DebugInfo.h"
#include "soaap.h"

using namespace soaap;

void ClassifiedAnalysis::initialise(ValueList& worklist, Module& M) {

  // initialise with pointers to annotated fields and uses of annotated global variables
  if (Function* F = M.getFunction("llvm.ptr.annotation.p0i8")) {
    for (User::use_iterator u = F->use_begin(), e = F->use_end(); e!=u; u++) {
      User* user = u.getUse().getUser();
      if (isa<IntrinsicInst>(user)) {
        IntrinsicInst* annotateCall = dyn_cast<IntrinsicInst>(user);
        Value* annotatedVar = dyn_cast<Value>(annotateCall->getOperand(0)->stripPointerCasts());

        GlobalVariable* annotationStrVar = dyn_cast<GlobalVariable>(annotateCall->getOperand(1)->stripPointerCasts());
        ConstantDataArray* annotationStrValArray = dyn_cast<ConstantDataArray>(annotationStrVar->getInitializer());
        StringRef annotationStrValCString = annotationStrValArray->getAsCString();
        
        if (annotationStrValCString.startswith(CLASSIFY)) {
          StringRef className = annotationStrValCString.substr(strlen(CLASSIFY)+1); //+1 because of _
          ClassifiedUtils::assignBitIdxToClassName(className);
          int bitIdx = ClassifiedUtils::getBitIdxFromClassName(className);
        
          dbgs() << "   Classification annotation " << annotationStrValCString << " found:\n";
        
          worklist.push_back(annotatedVar);
          state[annotatedVar] |= (1 << bitIdx);
        }
      }
    }
  }
  
  // annotations on variables are stored in the llvm.global.annotations global
  // array
  if (GlobalVariable* lga = M.getNamedGlobal("llvm.global.annotations")) {
    ConstantArray* lgaArray = dyn_cast<ConstantArray>(lga->getInitializer()->stripPointerCasts());
    for (User::op_iterator i=lgaArray->op_begin(), e = lgaArray->op_end(); e!=i; i++) {
      ConstantStruct* lgaArrayElement = dyn_cast<ConstantStruct>(i->get());

      // get the annotation value first
      GlobalVariable* annotationStrVar = dyn_cast<GlobalVariable>(lgaArrayElement->getOperand(1)->stripPointerCasts());
      ConstantDataArray* annotationStrArray = dyn_cast<ConstantDataArray>(annotationStrVar->getInitializer());
      StringRef annotationStrArrayCString = annotationStrArray->getAsCString();
      if (annotationStrArrayCString.startswith(CLASSIFY)) {
        GlobalValue* annotatedVal = dyn_cast<GlobalValue>(lgaArrayElement->getOperand(0)->stripPointerCasts());
        if (isa<GlobalVariable>(annotatedVal)) {
          GlobalVariable* annotatedVar = dyn_cast<GlobalVariable>(annotatedVal);
          if (annotationStrArrayCString.startswith(CLASSIFY)) {
            StringRef className = annotationStrArrayCString.substr(strlen(CLASSIFY)+1);
            ClassifiedUtils::assignBitIdxToClassName(className);
            state[annotatedVar] |= (1 << ClassifiedUtils::getBitIdxFromClassName(className));
          }
        }
      }
    }
  }

}

void ClassifiedAnalysis::postDataFlowAnalysis(Module& M) {
  // validate that classified data is never accessed inside sandboxed contexts that
  // don't have clearance for its class.
  for (Function* F : sandboxedMethods) {
    DEBUG(dbgs() << "Function: " << F->getName() << ", clearances: " << ClassifiedUtils::stringifyClassNames(sandboxedMethodToClearances[F]) << "\n");
    for (BasicBlock& BB : F->getBasicBlockList()) {
      for (Instruction& I : BB.getInstList()) {
        DEBUG(dbgs() << "   Instruction:\n");
        DEBUG(I.dump());
        if (LoadInst* load = dyn_cast<LoadInst>(&I)) {
          Value* v = load->getPointerOperand();
          DEBUG(dbgs() << "      Value:\n");
          DEBUG(v->dump());
          DEBUG(dbgs() << "      Value classes: " << state[v] << ", " << ClassifiedUtils::stringifyClassNames(state[v]) << "\n");
          if (!(state[v] == 0 || state[v] == sandboxedMethodToClearances[F] || (state[v] & sandboxedMethodToClearances[F]))) {
            outs() << " *** Sandboxed method \"" << F->getName() << "\" read data value of class: " << ClassifiedUtils::stringifyClassNames(state[v]) << " but only has clearances for: " << ClassifiedUtils::stringifyClassNames(sandboxedMethodToClearances[F]) << "\n";
            if (MDNode *N = I.getMetadata("dbg")) {
              DILocation loc(N);
              outs() << " +++ Line " << loc.getLineNumber() << " of file "<< loc.getFilename().str() << "\n";
            }
            outs() << "\n";
          }
        }
      }
    }
  }
}
