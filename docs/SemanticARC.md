
# Semantic ARC

## Preface

This is a proposal for a series of changes to the SIL IR in order to ease the optimization of ARC operations and allow for static verification of ARC semantics in SIL. This is a proposal meant for compiler writers and implementors, not users, i.e. we assume that the reader has a basic familiarity with the basic concepts of ARC.

**NOTE** We are talking solely about ARC as implemented beginning in Objective C. There may be other ARC implementations that are unknown to the writer.

## Historical Implementations

The first historical implementation of ARC in a mid level IR is in LLVM IR for use by the Objective C language. In this model, a pointer with ARC semantics is represented as an i8*. The lifetimes of the pointer is managed via retain and release operations and the end of the pointer's lifetime is ascertained via conservative analysis of uses. The retain, release calls, since at the LLVM IR level were just calls, did not have any semantic ARC information that enabled reasoning about what the corresponding balancing operation opposing the function call was. Additionally, there were many operations on ARC pointers that /should/ have resulted in atomic operations were instead separated into separate uses (i.e. load/store strong when compiling for optimization). Uses of Objective C pointers by functions were problematic as well since there was no verification of the semantic ARC convention that a function required of its pointer arguments. Finally, one could only establish that two operations on different pointers had the same RC identity conservatively via alias analysis. This prevents semantic guarantees from the IR in terms of ability to calculate RC identity.

The ARC implementation in Swift, in contrast, to Objective C, is implemented in the SIL IR. This suffered from the same issues as the Objective C implementation of ARC with one exception: argument conventions on function signatures. In SIL, all functions specify the ownership convention expected of their arguments and return values. Since these conventions were not specified in the operations in the bodies of functions though, this could not be used to create a true ARC verifier.

## Semantic ARC

As discussed in the previous section, the implementation of ARC in both Swift and Objective C lacked important semantic ARC information. We fix these issues by embedding the following ARC semantic information into SIL in the following order of implementation.

1. RC Identity: For any given SILValue, one should be able to determine its set of RC Identity Roots.
2. Additional High Level ARC Operations: store_strong, load_strong, copy_value instructions should be added to SIL.
3. Operand ARC Conventions: Function signature ARC conventions should be extended to all instructions.
4. ARC Verifier: An ARC verifier should be written that uses RC Identity, Operand ARC Conventions, and High Level ARC operations to statically verify that a program obeys ARC semantics.
5. Elimination of Memory Locations from High Level SIL. Memory locations should be represented as SSA values instead of memory locations. This will allow for address only values to be manipulated and have their lifetimes verified just like normal class types.

Swift Extensions:

1. One should be able to specify the parameter convention of **all** function parameters.

We now go into depth on each one of those points.

### Reference Count Identity Problem

In order to pair semantic ARC operations effectively, one has to be able to determine that two ARC operations are manipulating the same reference count. Currently in SIL this is not a robust operation due to the lack of IR level model of RC identity that is guaranteed to be preserved by the frontend and all emitted instructions. We wish to define an algorithm which for any specific SILValue in a given program can determine the set of "RC Identity Sources" associated with the given SILValue. We do this as follows: Define an RC Identity as a tuple consisting of a SILValue and a ProjectionPath to a leaf reference type in the SILValue. Then define an RC Identity Source via the following recursive relation:

1. rcidsource(a: SILArgument) consists of the list of all leaf reference types in a with the associated projection paths into a. *NOTE* This implies that by default, we consider SILArguments that act as phi nodes to block further association with rc identity sources.
2. To calculate the rcidsource of an instruction

### Pairing Semantic ARC Operations

## Solving the Reference Count Identity Problem

### Add RC Identity semantics to use-def chains

### Create an RC Identity Verifier

## Solving the Pairing Problem

### Transition to copy_value

### Add Ownership Semantics to use-def chains

### Create an Ownership Semantic Verifier
