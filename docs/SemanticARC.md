
# Semantic ARC

## Preface

This is a proposal for a series of changes to the SIL IR in order to ease the optimization of ARC operations and verification of ARC semantics in Swift programs. We assume that the user has a basic familiarity with the basic concepts of ARC: this is a proposal meant for compiler writers and implementors.

## Historical Implementations

ARC was first implemented for Objective C. In Objective C, Pointers with ARC semantics are represented in LLVM IR as i8*. The lifetimes of these pointers were managed via retain and release operations and the end of a pointer's lifetime was ascertained via conservative analysis of uses. The retain, release calls did not have any semantic information in the IR itself that showed what operations they were balancing and often times uses of ARC pointers that /should/ have resulted in atomic uses were separated into separate uses. Uses of Objective C pointers by functions were problematic as well since there was no verification of the semantic ARC convention that a function required of its pointer arguments. Finally, one could only establish that two pointers had the same RC Identity conservatively via alias analysis.

The ARC implementation in Swift improved upon this situation by specifying ARC semantic properties at the function level. For instance,

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
