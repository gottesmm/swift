
# SIL Ownership Model

In this document, we define a SIL Ownership Model. We assume that the reader is
already familiar with SIL, the concept of an ownership producer, ownership
consumer, and Ownership Identity.


Our goals are to ensure that
for any SSA value, the following must be true:

1. All "producer" instructions and "consumer" instructions of a given value are
   able to be found trivially via an API. This will be verified 

1. Explicit ownership identity propagation,
2. The ability to reason about ownership via use-def lists.
3. Explicit/statically verified ownership conventions on use-def edges. 

We do this by performing the following transformations:

1. Replace Low Level ARC Operations with High Level ARC Operations.
2. Define the "Ownership Identity" and the "SILValue Ownership Identity Root
   Algorithm" and enforce this via a Verifier.
3. Define the Ownership Verification Algorithm and enforce it in the SIL
   verifier as a property of the IR.
4. Represent Address Only Types via SSA values instead of Memory Locations to
   enable Address Only Types to participate in this system.

Once these four points have been implemented, Ownership semantics at the SIL
level will be statically verified.

### Replace Low Level ARC Operations with High Level ARC Operations

Most of the main ARC optimization primitives.  Many of the ARC operations used
in SIL today require are partial operations or do not allow for use-def lists
for reasoning about pairings/effects. dataflow to determine pairings/effects and
or are partial operations. Specifically:

1. **Replace strong_retain, retain_value with copy_value and strong_release,
   release_value with destroy_value**. The copy_value instruction will be
   semantically equivalent to performing a bit by bit copy of the input argument
   followed by a retain_value operation. The destroy_value instruction will be
   semantically equivalent to performing a release_value on its input value
   followed by a scribbling over the value with a known bit string when
   compiling with -Onone. When compiling with optimizations, no scribbling
   operation will be emitted. The benefit that copy_value and destroy_value
   provide over the other given instructions are that they enable ARC semantics
   to be encoded

2. strong store/strong load operations should be provided as instructions. This
   allows for normal loads to be considered as being unsafe unowned significant
   operations and eliminates a hole in SIL ownership model where a pointer is
   partially initialized (i.e. it a value is loaded but it has not been
   retained. In the time period in between those two points the value is
   partially initialized allowing for optimizer bugs).

### Ownership Identity and the Ownership Identity Verification Algorithm

One 

Once High Level SIL has been implemented, we will embed RC Identity into High
Level SIL to ensure that RC identity can always be computed for all SSA
values. Currently in SIL this is not a robust operation due to the lack of IR
level model of RC identity that is guaranteed to be preserved by the frontend
and all emitted instructions. Define an RC Identity as a tuple consisting of a
SILValue, V, and a ProjectionPath, P, to from V's type to a sub reference type
in V [[2]](#footnote-2). We wish to define an algorithm that given any (V, P) in
a program can determine the RC Identity Source associated with (V, P). We do
this recursively follows:

Let V be a SILValue and P be a projection path into V. Then:

1. If V is a SILArgument, then (V, P) is an RC Identity Source. *NOTE* This
   implies that by default, SILArguments that act as phi nodes are RC Identity
   Sources.
2. If V is the result of a SILInstruction I, then if I does not have any
   operands, (V, P) is an RC Identity Source. If I does have SILOperands then I
   must define how (V, P) is related to its operands. Some possible
   relationships are:
   
  i. RC Identity Forwarding. If I is a forwarding instruction, then (V, P) to an
  analogous RC Identity (OpV, OpP). Some examples of this sort of operation are
  casts, value projections, and value aggregations.
  
  ii. RC Identity Introducing. These are instructions which introduce new RC
  Identity values implying that (V, P) is an RC Identity Source. Some examples
  of these sorts of instructions are: apply, partial_apply.
  
  iii. Unspecified. If I is not an introducer or a forwarder and does not
  specify any specific semantics, then its RC Identity behavior is unspecified.

Our algorithm then is a very simple algorithm that applies the RC Identity
Source algorithm to all SSA values in the program and ensures that RC Identity
Sources can be computed for them. This should result in trivial use-def list
traversal.

### Endow Use-Def edges with ARC Conventions

Once we have these higher level operations, the next step is to create the
notion of operand and result ARC conventions for all instructions. At a high
level this is just the extension of argument/result conventions from apply sites
to /all/ instructions. By then verifying that each use-def pair have compatible
result/operand conventions, we can statically verify that ARC relationships are
being preserved.

In order to simplify this, we will make the following changes:

1. All SILInstructions must assign to their operands one of the following conventions:
   * @owned
   * @guaranteed
   * @unowned @safe
   * @unowned @unsafe
   * @forwarding

2. All SILInstructions must assign to their result one of the following conventions:
   * @owned
   * @unowned @unsafe
   * @unowned @safe
   * @forwarding

3. All SILArguments must have one of the following conventions associated with it:
   * @owned
   * @guaranteed
   * @unowned @unsafe
   * @unowned @safe
   * @forwarding

@forwarding is a new convention that we add to reduce the amount of extra
instructions needed to implement this scheme. @forwarding is a special
convention intended for instructions that forward RC Identity that for
simplictuy will be restricted to forwarding the convention of their def
instruction to all of the uses of that instruction. Of course, for forwarding
instructions with multiple inputs, we require that all of the inputs have the
same convention.

The general rule is that each result convention with the name x, must be matched
with the operand convention with the same name with some specific
exceptions. Let us consider an example. Consider the struct Foo:

    struct Foo {
      var x: Builtin.NativeObject
      var y: Builtin.NativeObject
    }

and the following SIL:

    sil @UseFoo : $@convention(thin) (@guaranteed Foo, @owned Foo) -> (@owned Builtin.NativeObject)
    
    sil @foo : $@convention(thin) (@guaranteed Builtin.NativeObject) -> () {
    bb0(%0 : @guaranteed $Builtin.NativeObject):
      %1 = function_ref @UseFoo : $@convention(thin) (@guaranteed Foo, @owned Foo) -> (@owned Builtin.NativeObject)

      # This is forwarding, so it is @guaranteed since %0 is guaranteed.
      %2 = struct $Foo(%0 : $Builtin.NativeObject, %0 : $Builtin.NativeObject)

      # Then we use copy_value to convert our @guaranteed def to an @owned use. copy_value is a converter instruction that converts 
      # any @owned or @guaranteed def to an @owned def.
      %3 = copy_value %2 : $Foo

      # %1 is called since we pass in the @guaranteed def, @owned def to the @guaranteed, @owned uses.
      %4 = apply %1(%2, %3) : $@convention(thin) (@guaranteed Foo, @owned Foo) -> (@owned Builtin.NativeObject) # This needs to be consumed
      destroy_value %4 : $@owned Builtin.NativeObject
      %5 = tuple()
      return %5 : $()
    }

Let us consider another example that is incorrect and where the conventions
allow for optimizer or frontend error to be caught easily. Consider foo2:

    sil @foo2 : $@convention(thin) (@guaranteed Builtin.NativeObject) -> () {
    bb0(%0 : @guaranteed $Builtin.NativeObject):
      %1 = function_ref @UseFoo : $@convention(thin) (@guaranteed Foo, @owned Foo) -> (@owned Builtin.NativeObject)
      
      # Guaranteed due to forwarding.
      %2 = struct $Foo(%0 : $Builtin.NativeObject, %0 : $Builtin.NativeObject)
      
      # ERROR! Passed an @guaranteed definition to an @owned use!
      %3 = apply %1(%2, %2) : $@convention(thin) (@guaranteed Foo, @owned Foo) -> (@owned Builtin.NativeObject)
      
      destroy_value %3 : $Builtin.NativeObject
      %4 = tuple()
      return %4 : $()
    }

In this case, since the apply's second argument must be @owned, a simple use-def
type verifier would throw, preventing an improper transfer of ownership. Now let
us consider a simple switch enum statement:

    sil @switch : $@convention(thin) (@guaranteed Optional<Builtin.NativeObject>) ->  () {
    bb0(%0 : $Optional<Builtin.NativeObject>):
      switch_enum %0, bb1: .Some, bb2: .None
      
    bb1(%payload : $Builtin.NativeObject):
      destroy_value %payload : $Builtin.NativeObject
      br bb3
      
    bb2:
      br bb3
      
   bb3:
      %result = tuple()
      return %result : $()
    }

While this may look correct to the naked eye, it is actually incorrect even in
SIL today. This is because in SIL today, switch_enum always takes arguments at
+1! Yet, in the IR there is no indication of the problem and the code will
compile! Now let us update the IR given semantic ARC:

    sil @switch : $@convention(thin) (@guaranteed Optional<Builtin.NativeObject>) ->  () {
    bb0(%0 : @guaranteed $Optional<Builtin.NativeObject>):
      switch_enum %0, bb1: .Some, bb2: .None
      
    # ERROR! Passing an @guaranteed def to an @owned use.
    bb1(%payload : @owned $Builtin.NativeObject):
      destroy_value %payload : $Builtin.NativeObject
      br bb3
      
    bb2:
      br bb3
      
    bb3:
      %result = tuple()
      return %result : $()
    }

A linear checker would automatically catch such an error and even more
importantly there are visual cues for the compiler engineer that the switch enum
argument needs to be a +1. We can fix this by introducing a copy_value.

    sil @switch : $@convention(thin) (@guaranteed Optional<Builtin.NativeObject>) ->  () {
    bb0(%0 : @guaranteed $Optional<Builtin.NativeObject>):
      # Change %1 from being an @guaranteed def to an @owned def.
      %2 = copy_value %1 : $Optional<Builtin.NativeObject>
      # Pass in the @owned def into the switch enum's @owned use.
      switch_enum %1, bb1: .Some, bb2: .None
      
    bb1(%payload : @owned $Builtin.NativeObject):
      destroy_value %payload : $Builtin.NativeObject
      br bb3
      
    bb2:
      br bb3
      
    bb3:
      %result = tuple()
      return %result : $()
    }

Then this will compile in the semantic ARC world. Let us consider how we could
convert the @owned switch_enum parameter to be @guaranteed. What does that even
mean. Consider the following switch enum example.

    class C {
       var foo: Optional<Builtin.NativeObject>
    }
     
    sil @switch : $@convention(thin) (@guaranteed C) ->  () {
    bb0(%0 : @guaranteed $C):
      %1 = ref_element_addr $C : #C.foo
      %2 = load_strong %1 : $*Builtin.NativeObject
      switch_enum %2, bb1: .Some, bb2: .None
      
    bb1(%payload : @owned $Builtin.NativeObject):
      destroy_value %payload : $Builtin.NativeObject
      br bb3
      
    bb2:
      br bb3
      
    bb3:
      %result = tuple()
      return %result : $()
    }

This is correct. But what if we want to get rid of the destroy_value by
performing @owned -> @guaranteed optimization. We do this first by converting
the switch_enum's parameter from being @owned to being @guaranteed.

    sil @switch : $@convention(thin) (@owned Optional<Builtin.NativeObject>) ->  () {
    bb0(%0 : @owned $Optional<Builtin.NativeObject>):
      # Convert the @owned def to an @guaranteed def.
      %1 = guarantee_lifetime %0 : $Optional<Builtin.NativeObject>
      # Pass in the @guaranteed optional to the switch.
      switch_enum %1, bb1: .Some, bb2: .None
      
    # NO ERROR!
    bb1(%payload : @guaranteed $Builtin.NativeObject):
      br bb3
      
    bb2:
      br bb3
      
    bb3:
      # End the guaranteed lifetime and convert the object back to @owned.
      %2 = destroy_lifetime_guarantee %1 : $Optional<Builtin.NativeObject>
      # Destroy the @owned parameter.
      destroy_value %2 : $Builtin.NativeObject
      %result = tuple()
      return %result : $()
    }

The key reason to have the guarantee_lifetime/destroy_lifetime_guarantee is that
it encapsulates via the use-def list the region where the lifetime of the object
is guaranteed. Once this is done, we then perform the @owned -> @guaranteed
optimization [[3]](#footnote-3):

    sil @switch : $@convention(thin) (@guaranteed Optional<Builtin.NativeObject>) ->  () {
    bb0(%0 : @guaranteed $Optional<Builtin.NativeObject>):
      # Convert the @guaranteed argument to an @owned def.
      %1 = copy_value %0 : $Optional<Builtin.NativeObject>
       
      # Convert the @owned def to an @guaranteed def.
      %2 = guarantee_lifetime %1 : $Optional<Builtin.NativeObject>
      
      # Pass in the @guaranteed optional to the switch.
      switch_enum %2, bb1: .Some, bb2: .None
      
    bb1(%payload : @guaranteed $Builtin.NativeObject):
      br bb3
      
    bb2:
      br bb3
      
    bb3:
      # End the guaranteed lifetime and convert the object back to @owned.
      %2 = destroy_lifetime_guarantee %1 : $Optional<Builtin.NativeObject>
      # Destroy the @owned parameter.
      destroy_value %2 : $Builtin.NativeObject
      %result = tuple()
      return %result : $()
    }

Once this has been done, we can then optimize via use-def lists by noticing that
the @owned parameter that we are converting to guaranteed was original
@guaranteed. In such a case, the copy is not necessary. Thus we can rewrite %2
to refer to %0 and rewrite the destroy_value in BB3 to refer to %1 and eliminate
the lifetime guarantee instructions, i.e.:

    sil @switch : $@convention(thin) (@guaranteed Optional<Builtin.NativeObject>) ->  () {
    bb0(%0 : @guaranteed $Optional<Builtin.NativeObject>):
      # Convert the @guaranteed argument to an @owned def.
      %1 = copy_value %0 : $Optional<Builtin.NativeObject>
      
      # Pass in the @guaranteed optional to the switch.
      switch_enum %0, bb1: .Some, bb2: .None
      
    bb1(%payload : @guaranteed $Builtin.NativeObject):
      br bb3
      
    bb2:
      br bb3
      
    bb3:
      # Destroy the @owned parameter.
      destroy_value %1 : $Builtin.NativeObject
      %result = tuple()
      return %result : $()
    }

Then we have a dead copy of a value that can thus be eliminated yielding the
following perfectly optimized function:

    sil @switch : $@convention(thin) (@guaranteed Optional<Builtin.NativeObject>) ->  () {
    bb0(%0 : @guaranteed $Optional<Builtin.NativeObject>):
      # Pass in the @guaranteed optional to the switch.
      switch_enum %0, bb1: .Some, bb2: .None
      
    bb1(%payload : @guaranteed $Builtin.NativeObject):
      br bb3
      
    bb2:
      br bb3
      
    bb3:
      %result = tuple()
      return %result : $()
    }

Beautiful.

### ARC Verifier

Once we have endowed use-def edges with ARC semantic properties, we can ensure
that ARC is statically correct by ensuring that for all function bodies the
following is true:

a. Every use-def edge must connect together a use and a def with compatible ARC
semantics. As an example this means that any def that produces a +1 value must
be paired with a -1 use. If one wishes to pass off a +1 value to an unowned use
or a guaranteed use, one must use an appropriate conversion instruction. The
conversion instruction would work as a pluggable adaptor and only certain
adaptors that preserve safe ARC semantics would be provided.

b. Every +1 operation can only be balanced by a -1 once along any path through
the program. This would be implemented in the verifier by using the use-def list
of a +1, -1 to construct joint-domination sets. The author believes that there
is a simple algorithm for disproving joint dominance of a set by an instruction,
but if one can not be come up with, there is literature for computing
generalized dominators that can be used. If computation of generalized
dominators is too expensive for normal use, they could be used on specific
verification bots and used when triaging bugs.

This guarantees via each instruction's interface that each +1 is properly
balanced by a -1 and that no +1 is balanced multiple times along any path
through the program... that is the program is ARC correct = ).

### Elimination of Memory Locations from High Level SIL

## Semantic ARC Based Optimization

With this data, new and novel forms of optimization are now possible. We present
an algorithm called the Signature Sequence Algorithm. Considering the world of
solely static functions and linkage unit visibility. In such a world, all
dynamic lifetimes of all objects across all region boundaries will be 1 as a
result of this program.

### "The Signature Optimization"

Regions of lifetimes are determined solely by polymorphism (i.e. on
restrained by polymorphism). Everything else can be specialized as
appropriate. Similar to strongly connected components. If one images the world
of lifetimes, there is a minimal lifetime starting from a polymorphic function
that is open. The reason why this is true is since one can not know everything
that needs to be specialized. Or if from Storage. We can have loading have
conventions. When one proves that there is a dominating lifetime, one changes
storage signature to be +0.

### "The Cleanup"

Otherwise, one could perform offsetting retains, releases so that each +1, +1 is
at the same scope. Then run the cleanup crew.

## Implementation

### Phase 1. Preliminaries

#### Parallel Task 1. Introduce new High Level Instructions. Can be done independently.

This one should be simple to do. Could give to Roman.

#### Parallel Task 2. Introduction of RC Identity Verification and RC Identity Sources.

Here we basically fix any issues that come up in terms of RC Identity not
propagating correctly. To test this, we make RCIdentityAnalysis use it so
everything just plugs in.

#### Parallel Task 3. Implement use-def list convention and convention verification.

Once this task is complete, we know that all use-def lists in the program are
correct.

##### Subtask a. Introduction of signatures to all block arguments without verification.

At this point in time, this work will be down on the SILParser/SILPrinter side
and making sure that it serializes properly and everything.

##### Subtask b. Introduce the notion of signatures to use-def lists.

Again, this would not be verified. This is where we would not wire anything up
to it.

##### Subtask c. We create a whitelist of instructions with unaudited use-def lists audited instructions and use it to advance incrementally fixing instructions.

We visit each instruction. If the instruction is not in the white list, we skip
it. If the instruction is in the white list, we check its value and its
users. If any user is not in the whitelist, we do not check the
connection. Before, we know everything we get far less coverage. Lets just check
if for all instructions... Done!

#### Parallel Task 4. Elimination of memory locations from High Level SIL.

Add any missing instructions. Add SIL level address only type.

**TODO: ADD SIL EXAMPLE HERE**

### Phase 2. Create Uses of Instrastructure

These run in order bottom up.

#### Parallel Task 1. Create Lifetime Verification algorithm.

The way this works is that we create an analysis of "verified" good
instructions. Then they all go away.

#### Parallel Task 2. Optimization: Create Lifetime Joining algorithm.

Then one can create the lifetime joining algorithm. This takes all of the
copy_addr and discovers any that *could* be joined, i.e. have the same parent
value and the copied value has not been written to. In the case of a pointer,
this is always safe to do.

Could run lifetime joining as a guaranteed pass. That suggests to me a minimal
thing and that the lifetime joining should happen before the copy propagation.

#### Parallel Task 3. Optimization: Extend Function Signature Optimizer -> Owner Signature Optimizer

**ROUGH NOTES**

1. Define an owner signature as (RC Identity, Last Ownership Start Equivalence
   Class). Last Ownership Start Equivalence class are the lists of how ownership
   changes. Since we have ownership on PHI arguments, life is good, i.e. no node
   can ever have a non-rc identity source.

1. Create a (RC Identity, Last Ownership Start) Graph. This is a list defined by
   the equivalence class of regions that forward from a specific rc identity and
   ownership definition of foo.
2. Create a graph on these tuples where there is an arrow from (RC Identity,
   Last Ownership Start[n]) -> (RC Identity, Last Ownership Start[n+1])
   i.e. where a new value is introduced.
3. If you view each thing as a safe copy, then if Last Ownership Start[n] ->
   Last Ownership Start[n+1] has the same convention, one can forward.
4. Loading from storage and writing from storage is signature?!

Think of new definitions as new start regions and copies as new start
regions. In a way, those are signatures. One could abstract that to ownership
perhaps?

1. Create Region Signature Graph annotated where you have 2 types of
   nodes. Signature nodes and argument nodes.
2. Any Signature node's def, if it has the same def as that signature and there
3. Flip colored graph.

Refactor function signature optimization to be able to apply to function
arguments and block arguments. When visiting a function, start visiting blocks
and fix up their signatures and then fix up function signatures. Do all of this
bottom up.

Can use Loop Information to reason about loops.

#### Parallel Task 4. Optimization Copy Propagation

Color regions of ownership by if it is +1 or not +1. Things that are
not-polymorphic can not cause a retain/release to occur. That would be an
amazing thing to be able to prove. The rule would be that a copy could only come
from a polymorphic unknown type or a load from an internal in that specific
lifetime value.

## Footnotes

<a name="footnote-1">[1]</a> Reference Count Identity ("RC Identity") is a
concept that is independent of pointer identity that refers to the set of
reference counts that would be manipulated by a reference counted operation upon
a specific SSA value. For more information see the
[RC Identity](https://github.com/apple/swift/blob/master/docs/ARCOptimization.rst#rc-identity)
section of the
[ARC Optimization guide](https://github.com/apple/swift/blob/master/docs/ARCOptimization.rst)

<a name="footnote-2">[2]</a> **NOTE** In many cases P will be the empty set
(e.g. the case of a pure reference type)

<a name="footnote-3">[3]</a> **NOTE** This operation is only done in
coordination with inserting a destroy_value into callers of @switch.
<!--

1. **Split the Canonical SIL Stage into High and Low Level SIL**: High Level SIL
   will be the result of running the guaranteed passes and is where ARC
   invariants will be enforced.


### High Level SIL and Low Level SIL

The first step towards implementing Semantic ARC is to split the "Canonical SIL
Stage" into two different stages: High Level and Low Level SIL. The main
distinction in between the two stages is, that in High Level SIL, ARC semantic
invariants will be enforced via extra conditions on the IR. In contrast, once
Low Level SIL has been reached, no ARC semantic invariants are enforced and only
very conservative ARC optimization may occur. The intention is that Low Level
SIL would /only/ be used when compiling with optimization enabled, so both High
and Low Level SIL will necessarily need to be able to be lowered to LLVM IR.


# Swift Extensions:

1. One should be able to specify the parameter convention of **all** function parameters. -->


<!--

## Definitions

Some definitions for those who are unfamiliar:

1. **Non-Trivial Value**. A non-trivial value is a value that contains a value
   with some sort of ownership semantics. By containing a value with ownership
   semantics, the compiler must treat the non-trivial value as a value with the
   union of the semantics of each of its parts.

2. **Ownership Identity Root**. The ownership identity root of an SSA value is
   defined as a pair consisting of a projection path into a type combined with a
   side-effect having instruction that side-effect having instruction that
   "produces" a new SSA value with an independent lifetime...

**TODO IS THIS NECESSARY**
-->
