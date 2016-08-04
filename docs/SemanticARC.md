
# Semantic ARC

## Preface

This is a proposal for a series of changes to the SIL IR in order to ease the optimization of ARC operations and allow for static verification of ARC semantics in SIL. This is a proposal meant for compiler writers and implementors, not users, i.e. we assume that the reader has a basic familiarity with the basic concepts of ARC.

**NOTE** We are talking solely about ARC as implemented beginning in Objective C. There may be other ARC implementations that are unknown to the writer.

## Historical Implementations

The first historical implementation of ARC in a mid level IR is in LLVM IR for use by the Objective C language. In this model, a pointer with ARC semantics is represented as an i8*. The lifetimes of the pointer is managed via retain and release operations and the end of the pointer's lifetime is ascertained via conservative analysis of uses. retain, release instructions were just calls that did not have any semantic ARC information that enabled reasoning about what the corresponding balancing operation opposing it was. Additionally, there were many operations on ARC pointers that /should/ have resulted in atomic operations were instead separated into separate uses (i.e. load/store strong when compiling for optimization). Uses of Objective C pointers by functions were problematic as well since there was no verification of the semantic ARC convention that a function required of its pointer arguments. Finally, one could only establish that two operations on different pointers had the same RC identity [[1]](#footnote-1) conservatively via alias analysis. This prevents semantic guarantees from the IR in terms of ability to calculate RC identity.

The ARC implementation in Swift, in contrast, to Objective C, is implemented in the SIL IR. This suffers from some of the same issues as the Objective C implementation of ARC but also provides some noteworthy improvements:

1. Reference Semantics in the Type System. Since SIL's type system is based upon the Swift type system, the notion of a reference countable type exists. This means that it is possible to verify that reference counting operations only apply to SSA values and memory locations associated reference counted types.
2. Argument conventions on function signatures. In SIL, all functions specify the ownership convention expected of their arguments and return values. Since these conventions were not specified in the operations in the bodies of functions though, this could not be used to create a true ARC verifier.

*TODO: I could point out here that many of the ideas in Semantic ARC are extending these ideas throughout the IR?*

## Semantic ARC

As discussed in the previous section, the implementation of ARC in both Swift and Objective C lacked important semantic ARC information. We fix these issues by embedding the following ARC semantic information into SIL in the following order of implementation:

1. **Split the Canonical SIL Stage into High and Low Level SIL**: High Level SIL will be the result of running the guaranteed passes and is where ARC invariants will be enforced.
2. **RC Identity**: For any given SILValue, one should be able to determine its set of RC Identity Roots. This makes it easy to reason about which reference counts a reference count operation is affecting.
3. **Introduction of new High Level ARC Operations**: store_strong, load_strong, copy_value instructions should be added to SIL. These operations are currently split into separate low level operations and are the missing pieces towards allowing all function local ARC relationships to be expressed via use-def chains. **TODO: ADD MORE HERE**
4. **Endow Use-Def edges with ARC Conventions**: Function signature ARC conventions should be extended to all instructions and block arguments. Thus all use-def edges should have an implied ownership transfer convention.
5. **ARC Verifier**: An ARC verifier should be written that uses RC Identity, Operand ARC Conventions, and High Level ARC operations to statically verify that a program obeys ARC semantics.

<!-- 6. **Elimination of Memory Locations from High Level SIL**. Memory locations should be represented as SSA values instead of memory locations. This will allow for address only values to be manipulated and have their lifetimes verified by the ARC verifier in a trivial way without the introduction of Memory SSA. -->

We now go into depth on each one of those points.

## High Level SIL and Low Level SIL

The first step towards implementing Semantic ARC is to split the "Canonical SIL Stage" into two different stages: High Level and Low Level SIL. The main distinction in between the two stages is, that in High Level SIL, ARC semantic invariants will be enforced via extra conditions on the IR. In contrast, once Low Level SIL has been reached, no ARC semantic invariants are enforced and only very conservative ARC optimization may occur. The intention is that Low Level SIL would /only/ be used when compiling with optimization enabled, so both High and Low Level SIL will necessarily need to be able to be lowered to LLVM IR.

## RC Identity

Once High Level SIL has been implemented, we will embed RC Identity into High Level SIL to ensure that RC identity can always be computed for all SSA values. Currently in SIL this is not a robust operation due to the lack of IR level model of RC identity that is guaranteed to be preserved by the frontend and all emitted instructions. Define an RC Identity as a tuple consisting of a SILValue, V, and a ProjectionPath, P, to from V's type to a sub reference type in V [[2]](#footnote-2). We wish to define an algorithm that given any (V, P) in a program can determine the RC Identity Source associated with (V, P). We do this recursively follows:

Let V be a SILValue and P be a projection path into V. Then:

1. If V is a SILArgument, then (V, P) is an RC Identity Source. *NOTE* This implies that by default, SILArguments that act as phi nodes are RC Identity Sources.
2. If V is the result of a SILInstruction I, then if I does not have any operands, (V, P) is an RC Identity Source. If I does have SILOperands then I must define how (V, P) is related to its operands. Some possible relationships are:
  i. RC Identity Forwarding. If I is a forwarding instruction, then (V, P) to an analogous RC Identity (OpV, OpP). Some examples of this sort of operation are casts, value projections, and value aggregations.
  ii. RC Identity Introducing. These are instructions which introduce new RC Identity values implying that (V, P) is an RC Identity Source. Some examples of these sorts of instructions are: apply, partial_apply.
  iii. Unspecified. If I is not an introducer or a forwarder and does not specify any specific semantics, then its RC Identity behavior is unspecified.

Our algorithm then is a very simple algorithm that applies the RC Identity Source algorithm to all SSA values in the program and ensures that RC Identity Sources can be computed for them. This should result in trivial use-def list traversal.

## New High Level ARC Operations

Once we are able to reason about 

## Endow Use-Def edges with ARC Conventions

## ARC Verifier

 by ensuring the following properties are true of all reference counting operations in a function body:

  a. Every use-def edge must connect together a use and a def with compatible ARC semantics. As an example this means that any def that produces a +1 value must be paired with a -1 use. If one wishes to pass off a +1 value to an unowned use or a guaranteed use, one must use an appropriate conversion instruction. The conversion instruction would work as a pluggable adaptor and only certain adaptors that preserve safe ARC semantics would be provided.
  
  b. Every +1 operation can only be balanced by a -1 once along any path through the program. This would be implemented in the verifier by using the use-def list of a +1, -1 to construct joint-domination sets. The author believes that there is a simple algorithm for disproving joint dominance of a set by an instruction, but if one can not be come up with, there is literature for computing generalized dominators that can be used. If computation of generalized dominators is too expensive for normal use, they could be used on specific verification bots and used when triaging bugs.

<a name="footnote-1">[1]</a> Reference Count Identity ("RC Identity") is a concept that is independent of pointer identity that refers to the set of reference counts that would be manipulated by a reference counted operation upon a specific SSA value. For more information see the [RC Identity](https://github.com/apple/swift/blob/master/docs/ARCOptimization.rst#rc-identity) section of the [ARC Optimization guide](https://github.com/apple/swift/blob/master/docs/ARCOptimization.rst)

<a name="footnote-2">[2]</a> **NOTE** In many cases P will be the empty set (e.g. the case of a pure reference type)

<!-- ## Elimination of Memory Locations from High Level SIL

# Swift Extensions:

1. One should be able to specify the parameter convention of **all** function parameters. -->
