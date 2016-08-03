
# Semantic ARC

## Preface

This is a proposal for a series of changes to the SIL IR in order to ease the optimization of ARC operations and verification of ARC semantics in Swift programs. We assume that the user has a basic familiarity with the basic concepts of ARC: this is a proposal meant for compiler writers and implementors.

## Historical Implementations

ARC was first implemented for Objective C. In Objective C, Pointers with ARC semantics are represented in LLVM IR as i8*. The lifetimes of these pointers were managed via retain and release operations and the end of a pointer's lifetime was ascertained via conservative analysis of uses. The retain, release calls did not have any semantic information in the IR itself that showed what operations they were balancing and often times uses of ARC pointers that /should/ have resulted in atomic uses were separated into separate uses. Uses of Objective C pointers by functions were problematic as well since there was no verification of the semantic ARC convention that a function required of its pointer arguments. Finally, one could only establish that two pointers had the same RC Identity conservatively via alias analysis. This prevents semantic guarantees from the IR in terms of ability to calculate RC identity.

The ARC implementation in Swift, in contrast, to Objective C, is implemented in the SIL IR. This suffered from many of the same issues as the Objective C implementation of ARC, with one exception: function signatures. In SIL, all functions specify the ownership convention expected of their arguments and return values. Since these conventions were not specified in the operations in the bodies of functions though, this could not be used to create a true ARC verifier.

## Semantic ARC

As shown in the past section, the implementation of ARC in both Swift and Objective C lacked important semantic information in the following areas:

1. Ability to determine semantic ARC pointer equivalence (RC Identity).
2. Ability to pair semantic ARC operations.

Our proposal solves these problems as follows:

#### Reference Count Identity Problem

#### Pairing Problems

## Solving the Reference Count Identity Problem

### Add RC Identity semantics to use-def chains

### Create an RC Identity Verifier

## Solving the Pairing Problem

### Transition to copy_value

### Add Ownership Semantics to use-def chains

### Create an Ownership Semantic Verifier
