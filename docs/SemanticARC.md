
# Semantic ARC

## Preface

This is a proposal for a series of changes to the SIL IR in order to ease the optimization of ARC operations and allow for static verification of ARC semantics in SIL. This is a proposal meant for compiler writers and implementors, not users, i.e. we assume that the reader has a basic familiarity with the basic concepts of ARC.

**NOTE** We are talking solely about ARC as implemented beginning in Objective C. There may be other ARC implementations that are unknown to the writer.

## Historical Implementations

The first historical implementation of ARC in a mid level IR is in LLVM IR for use by the Objective C language. In this model, a pointer with ARC semantics is represented as an i8*. The lifetimes of the pointer is managed via retain and release operations and the end of the pointer's lifetime is ascertained via conservative analysis of uses. retain, release instructions were just calls that did not have any semantic ARC information that enabled reasoning about what the corresponding balancing operation opposing it was. Additionally, there were many operations on ARC pointers that /should/ have resulted in atomic operations were instead separated into separate uses (i.e. load/store strong when compiling for optimization). Uses of Objective C pointers by functions were problematic as well since there was no verification of the semantic ARC convention that a function required of its pointer arguments. Finally, one could only establish that two operations on different pointers had the same RC identity conservatively via alias analysis. This prevents semantic guarantees from the IR in terms of ability to calculate RC identity.

The ARC implementation in Swift, in contrast, to Objective C, is implemented in the SIL IR. This suffers from some of the same issues as the Objective C implementation of ARC but also provides some noteworthy improvements:

1. Reference Semantics in the Type System. Since SIL's type system is based upon the Swift type system, the notion of a reference countable type exists. This means that it is possible to verify that reference counting operations only apply to SSA values and memory locations associated reference counted types.
2. Argument conventions on function signatures. In SIL, all functions specify the ownership convention expected of their arguments and return values. Since these conventions were not specified in the operations in the bodies of functions though, this could not be used to create a true ARC verifier.

*TODO: I could point out here that many of the ideas in Semantic ARC are extending these ideas throughout the IR?*

## Semantic ARC

As discussed in the previous section, the implementation of ARC in both Swift and Objective C lacked important semantic ARC information. We fix these issues by embedding the following ARC semantic information into SIL in the following order of implementation:

1. **Split the Canonical SIL Stage into High and Low Level SIL**: High Level SIL will be the result of running the guaranteed passes and is where ARC invariants will be enforced.
2. **RC Identity**: For any given SILValue, one should be able to determine its set of RC Identity Roots. This makes it easy to reason about which reference counts a reference count operation is affecting.
3. **Introduction of new High Level ARC Operations**: store_strong, load_strong, copy_value instructions should be added to SIL. These operations are currently split into separate low level operations. **TODO: ADD MORE HERE**
4. **Endow Use-Def edges with ARC Conventions**: Function signature ARC conventions should be extended to all instructions and block arguments. Thus all use-def edges should have an implied ownership transfer convention.
5. **ARC Verifier**: An ARC verifier should be written that uses RC Identity, Operand ARC Conventions, and High Level ARC operations to statically verify that a program obeys ARC semantics.
<!-- 6. **Elimination of Memory Locations from High Level SIL**. Memory locations should be represented as SSA values instead of memory locations. This will allow for address only values to be manipulated and have their lifetimes verified by the ARC verifier in a trivial way without the introduction of Memory SSA. -->

We now go into depth on each one of those points.

## High Level SIL and Low Level SIL

 ARC optimization /will/ not occur at the Low Level SIL. This implies that function signature optimization and IPO must occur at High Level SIL. Necessarily both High and Low Level SIL must be able to be lowered by IRGen to LLVM IR to ensure that 

## RC Identity

In order to pair semantic ARC operations effectively, one has to be able to determine that two ARC operations are manipulating the same reference count. Currently in SIL this is not a robust operation due to the lack of IR level model of RC identity that is guaranteed to be preserved by the frontend and all emitted instructions. We wish to define an algorithm which for any specific SILValue in a given program can determine the set of "RC Identity Sources" associated with the given SILValue. We do this as follows: Define an RC Identity as a tuple consisting of a SILValue and a ProjectionPath to a leaf reference type in the SILValue. Then define an RC Identity Source via the following recursive relation:

1. rcidsource(a: SILArgument) consists of the list of all leaf reference types in a with the associated projection paths into a. *NOTE* This implies that by default, we consider SILArguments that act as phi nodes to block further association with rc identity sources.
2. To calculate the rcidsource of an instruction

## New High Level ARC Operations

## Endow Use-Def edges with ARC Conventions

## ARC Verifier

 by ensuring the following properties are true of all reference counting operations in a function body:

  a. Every use-def edge must connect together a use and a def with compatible ARC semantics. As an example this means that any def that produces a +1 value must be paired with a -1 use. If one wishes to pass off a +1 value to an unowned use or a guaranteed use, one must use an appropriate conversion instruction. The conversion instruction would work as a pluggable adaptor and only certain adaptors that preserve safe ARC semantics would be provided.
  
  b. Every +1 operation can only be balanced by a -1 once along any path through the program. This would be implemented in the verifier by using the use-def list of a +1, -1 to construct joint-domination sets. The author believes that there is a simple algorithm for disproving joint dominance of a set by an instruction, but if one can not be come up with, there is literature for computing generalized dominators that can be used. If computation of generalized dominators is too expensive for normal use, they could be used on specific verification bots and used when triaging bugs.

## Elimination of Memory Locations from High Level SIL

# Swift Extensions:

1. One should be able to specify the parameter convention of **all** function parameters.
