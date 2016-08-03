
# Semantic ARC

## Preface

This is a proposal for a series of changes to the SIL IR in order to ease the optimization of ARC operations and verification of ARC semantics in Swift programs. This is a proposal meant for compiler writers and implementors, not users, i.e. we assume that the reader has a basic familiarity with the basic concepts of ARC.

*NOTE* We are talking solely about ARC as implemented beginning in Objective C. There may be other ARC implementations that are unknown to the writer.

## Historical Implementations

The first historical implementation of ARC in a mid level IR was in LLVM IR for use by the Objective C language. In this model, a pointer with ARC semantics is represented as an i8*. The lifetimes of the pointer is managed via retain and release operations and the end of the pointer's lifetime is ascertained via conservative analysis of uses. The retain, release calls, since at the LLVM IR level were just calls, did not have any semantic ARC information that enabled reasoning about what the corresponding balancing operation opposing the function call was. Additionally, there were many operations on ARC pointers that /should/ have resulted in atomic operations were instead separated into separate uses (i.e. load/store strong when compiling for optimization). Uses of Objective C pointers by functions were problematic as well since there was no verification of the semantic ARC convention that a function required of its pointer arguments. Finally, one could only establish that two operations on different pointers had the same RC identity conservatively via alias analysis. This prevents semantic guarantees from the IR in terms of ability to calculate RC identity.

The ARC implementation in Swift, in contrast, to Objective C, is implemented in the SIL IR. This suffered from the same issues as the Objective C implementation of ARC with one exception: argument conventions on function signatures. In SIL, all functions specify the ownership convention expected of their arguments and return values. Since these conventions were not specified in the operations in the bodies of functions though, this could not be used to create a true ARC verifier.

## Semantic ARC

As discussed in the previous section, the implementation of ARC in both Swift and Objective C lacked important semantic ARC information in the following areas:

1. Ability to determine semantic ARC pointer equivalence (RC Identity).
2. Ability to pair semantic ARC operations.
3. Lack of specific atomic ARC operations that provide atomic semantics related to initialization, transfering of ARC operations to and from memory.

We suggest in this proposal the following changes to the SIL IR as solutions to these problems:

### Reference Count Identity Problem

In order to pair semantic ARC operations effectively, one has to be able to determine that two ARC operations are manipulating the same reference count. Currently in SIL this is not a robust operation due to the lack of IR level model of RC identity that is preserved by the frontend and all emitted instructions. We wish to define an algorithm which for any specific SILValue in a given program can determine the set of "RC Identity Sources" associated with the given SILValue.

Let F be a given SIL function. Let V(F) be the set of SIL values in F and I(F) be the set of SIL instructions in F. There is a natural embedding of I(F) into V(F) defined by the function, value : I(F) -> V(F) defined by mapping each instruction in I(F) to its result value. Now define the predicate rcidsource : V -> Bool. Then define the set of rc identity source values as:

    RCIDSource  = { v ϵ V : rcidsource(v) }

We wish to attempt to define a function RCIDRoots : V(F) -> S with S ⊂ RCIDSource. In words this means that RCIDRoots must be able to map any SIL value v ϵ V(F) to a set of RCIDSource values. This algorithm is trivial to formulate when 

In order to solve this problem in a robust way in the face of compiler changes, we propose the following solution:

1. In SILNodes.def, all instructions will have attached to them a notion of whether or not the instruction can "produce" a new reference count identity. There will be 3 states: True, False, Special.
2. An API will be provided that for any specific SILValue returns the set of RC Identity roots associated with the given SILValue. All RC Identity Roots must be RC Identity root instructions.

### Pairing Semantic ARC Operations

## Solving the Reference Count Identity Problem

### Add RC Identity semantics to use-def chains

### Create an RC Identity Verifier

## Solving the Pairing Problem

### Transition to copy_value

### Add Ownership Semantics to use-def chains

### Create an Ownership Semantic Verifier
