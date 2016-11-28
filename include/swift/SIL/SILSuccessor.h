//===--- SILSuccessor.h - Terminator Instruction Successor ------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_SILSUCCESSOR_H
#define SWIFT_SIL_SILSUCCESSOR_H

#include "llvm/ADT/ilist.h"
#include "llvm/ADT/ilist_node.h"
#include <cassert>
#include <cstddef>
#include <iterator>

namespace swift {

class SILBasicBlock;
class TermInst;

/// This represents a reference to a SILBasicBlock in a terminator instruction,
/// forming a list of TermInst references to BasicBlocks.  This forms the
/// predecessor list, ensuring that it is always kept up to date.
class SILSuccessor : public llvm::ilist_node<SILSuccessor> {
  friend struct llvm::ilist_traits<SILSuccessor>;

  using ilist_traits = llvm::ilist_traits<SILSuccessor>;

  /// This is the Terminator instruction that contains this successor.
  TermInst *ContainingInst = nullptr;

  /// If non-null, this is the BasicBlock that the terminator branches to.
  SILBasicBlock *SuccessorBlock = nullptr;

public:
  SILSuccessor() {}

  SILSuccessor(TermInst *CI)
    : ContainingInst(CI) {
  }

  SILSuccessor(TermInst *CI, SILBasicBlock *Succ)
    : ContainingInst(CI) {
    *this = Succ;
  }
  
  ~SILSuccessor() {
    *this = nullptr;
  }

  void operator=(SILBasicBlock *BB);
  
  operator SILBasicBlock*() const { return SuccessorBlock; }
  SILBasicBlock *getBlock() const { return SuccessorBlock; }

  // Do not copy or move these.
  SILSuccessor(const SILSuccessor &) = delete;
  SILSuccessor(SILSuccessor &&) = delete;

  TermInst *getTerminator() const { return ContainingInst; }
};

} // end swift namespace

namespace llvm {

template <>
struct ilist_traits<::swift::SILSuccessor>
    : ilist_default_traits<::swift::SILSuccessor> {

  using SILSuccessor = ::swift::SILSuccessor;
  using SelfTy = ilist_traits<::swift::SILSuccessor>;
  using successor_iterator = simple_ilist<SILSuccessor>::iterator;

public:
  static void deleteNode(SILSuccessor *Succ) { Succ->~SILSuccessor(); }

  void addNodeToList(SILSuccessor *Succ) {}

  void transferNodesFromList(ilist_traits<SILSuccessor> &SrcTraits,
                             successor_iterator First, successor_iterator Last);

private:
  static void createNode(const SILSuccessor &);
};

} // end llvm namespace

#endif
