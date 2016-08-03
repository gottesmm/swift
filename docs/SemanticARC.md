
# Semantic ARC

## Preface

This is a proposal for a series of changes to the SIL IR in order to improve the ability for compiler writers/optimizers to verify and optimize the ARC operations. We assume that the user is familiar with the basic concepts of ARC as this is a proposal meant for compiler writers and implementors.

## Historical Notes

### Representation

ARC was first implemented for Objective C and is the basis for managing life times of reference types in Swift. In Objective C, ARC pointers are represented in LLVM IR as i8* pointers whose lifetimes were managed by retain and release function calls. These function calls do not necessarily have any semantic connection in the IR itself (i.e. one can not verify if two retain, release pairs are truly paired). In addition, two pointers could only be proven to be the same via conservative heuristics. The convention for passing an argument in Objective C was +0 so that was assumed by default. This caused issues since there was no true semantic verification that a value being passed off to a function was truly supposed to be passed at +0.

The ARC implementation in Swift improved upon these issues by attempting to begin specifying ARC relationships at the function boundary level. For instance, in Swift a function specifies the ownership convention that it expects each one of its arguments to 

### Optimization

Traditionally, ARC has been optimized via a bidirectional dataflow algorithm that attempts to prove that a set of retains and releases joint dominate/post-dominate each other. If the appropriate conditions are discovered during the dataflow then the retain/release sets are removed. Even though this algorithm is very powerful and removes most redundant retain/release pairings is does have several weaknesses:

1. It attempts to prove via various heuristics that applying ARC operations to two retainable values will manipulate the same reference count. This is not always easy to prove and in the face of a changing IR the lack of verification causes this to be brittle. In addition, if the heuristics are incorrect, then retain and release operations that do not always manipulate the same reference count. This would then result in a use after free.

2.

### ARC Problems

#### Reference Count Identity Problem

#### Pairing Problems

## Solving the Reference Count Identity Problem

### Add RC Identity semantics to use-def chains

### Create an RC Identity Verifier

## Solving the Pairing Problem

### Transition to copy_value

### Add Ownership Semantics to use-def chains

### Create an Ownership Semantic Verifier
