# SIL Programmers' Manual

This document provides information for developers working on the
implementation of SIL. The formal specification of the Swift
Intermediate _Language_ is in SIL.rst. This is a guide to the internal
implementation. Source comments normally provide this level of
information as much as possible. However, some features of the
implementation are spread out across the source base. This
documentation is meant to offer a central point for approaching the
source base with links to the code for more detailed comments.

## SILType

TBD: Define the different levels of types. Explain type lowering with
examples.

## SILInstructionResults

TBD: Explain how the various types fit together with pointers to the
source: SILValue, SILInstruction, SingleValueInstruction,
MultipleValueInstructionResult. And why it was done this way.

## `SILFunction` and `apply` arguments.

Throughout the compiler, integer indices are used to identify argument
positions in several different contexts:

- A `SILFunctionType` has a tuple of parameters.

- The SIL function definition has a list of `SILFunctionArgument`.
  This is the callee-side argument list. It includes indirect results.

- `apply`, `try_apply`, and `begin_apply` have "applied arguments":
  the subset of instruction operands representing the callee's
  `SILFunctionArgument` list.

- `partial_apply` has "applied arguments": the subset of instruction
  operands representing closure captures. Closure captures in turn map
  to a subset of the callee's `SILFunctionArgument` list.

- In the above three contexts, `SILFunctionArgument`, `apply`, and
  `partial_apply`, the argument indices also depend on the SIL stage:
  Canonical vs. Lowered.

Consider the example:

```
func example<T>(i: Int, t: T) -> (Int, T) {
  let foo = { return ($0, t) }
  return foo(i)
}
```

The closure `foo` has the indices as numbered below in each
context, ignoring the calling convention and boxing/unboxing of
captures for brevity:

The closure's `SILFunctionType` has two direct formal parameters at
indices (`#0`, `#1`) and one direct formal result of tuple type:

```
SILFunctionType(foo): (#0: Int, #1: T) -> @out (Int, T)
```

Canonical SIL with opaque values matches `SILFunctionType`. The
definition of `foo` has two direct `SILFunctionArgument`s at (`#0`,
`#1`):

```
SILFunctionArguments: (#0: Int, #1: T) -> (Int, T)
```

The Lowered SIL for `foo`s definition has an indirect "result
argument" at index #0. The function parameter indices are now (`#1`,
`#2`):

```
SILFunctionArguments: (#0: *T, #1: Int, #2: T) -> Int
```

Creation of the closure has one applied argument at index `#0`. Note
that the first applied argument is actually the second operand (the
first is the callee), and in lowered SIL, it is actually the third
SILFunctionArgument (after the indirect result and first parameter):

```
%closure = partial_apply @foo(#0: t)
```

Application of the closure with opaque values has one applied
argument:

```
%resultTuple = apply %closure(#0: i)
```

Lowered application of the closure has two applied arguments:

```
%directResult = apply %closure(#0: %indirectResult: *T, #1: i)
```

The mapping between `SILFunctionType` and `SILFunctionArgument`, which depends
on the SIL stage, is managed by the
[SILFunctionConventions](https://github.com/apple/swift/blob/master/include/swift/SIL/SILFunctionConventions.h)
abstraction. This API follows naming conventions to communicate the meaning of the integer indices:

- "Parameters" refer to the function signature's tuple of arguments.

- "SILArguments" refer to the set of `SILFunctionArgument` in the callee's entry block, including any indirect results required by the current SIL stage.

These argument indices and their relative offsets should never be
hardcoded. Although this is common practice in LLVM, it should be
avoided in SIL: (a) the structure of SIL instructions, particularly
applies, is much more nuanced than LLVM IR, (b) assumptions that may
be valid at the initial point of use are often copied into parts of
the code where they are no longer valid; and (c) unlike LLVM IR, SIL
is not stable and will continue evolving.

Translation between SILArgument and parameter indices should use:
`SILFunctionConventions::getSILArgIndexOfFirstParam()`.

Translation between SILArgument and result indices should use:
`SILFunctionConventions::getSILArgIndexOfFirstIndirectResult()`.

Convenience methods exist for the most common uses, so there is
typically no need to use the above "IndexOfFirst" methods to translate
one integer index into another. The naming convention of the
convenience method should clearly indicate which form of index it
expects. For example, information about a parameter's type can be retrieved directly from a SILArgument index: `getParamInfoForSILArg(index)`, `getSILArgumentConvention(index)`, and `getSILArgumentType(index)`.

Another abstraction,
[`ApplySite`](https://github.com/search?utf8=✓&q=%22class+ApplySite%22+repo%3Aapple%2Fswift+path%3Ainclude%2Fswift%2FSIL&type=Code&ref=advsearch&l=&l=),
abstracts over the various kinds of `apply` instructions, including
`try_apply`, `begin_apply`, and `partial_apply`.

`ApplySite::getSubstCalleeConv()` is commonly used to query the
callee's `SILFunctionConventions` which provides information about the
function's type and its definition as explained above. Information about the applied arguments can be queried directly from the `ApplySite` API.

For example, `ApplySite::getAppliedArgumentConvention(index)` takes an
applied argument index, while
`SILFunctionArguments::getSILArgumentConvention(index`) takes a
`SILFunctionArgument` index. They both return the same information,
but from a different viewpoint.

A common mistake is to directly map the ApplySite's caller-side
arguments onto callee-side SILFunctionArguments. This happens to work
until the same code is exposed to a `partial_apply`. Instead, use the `ApplySite` API for applied argument indices, or use
`ApplySite::getCalleeArgIndexOfFirstAppliedArg()` to translate the
apply's arguments into function convention arguments.

Consistent use of common idioms for accessing arguments should be
adopted throughout the compiler. Plenty of bugs have resulted from
assumptions about the form of SIL made in one area of the compiler
that have been copied into other parts of the compiler. For example,
knowing that a block of code is guarded by a dynamic condition that
rules out PartialApplies is no excuse to conflate applied arguments
with function arguments. Also, without consistent use of common
idioms, it becomes overly burdensome to evolve these APIs over time.

## SILGen

TBD: Possibly link to a separate document explaining the architecture of SILGen.

Some information from SIL.rst could be moved here.

## IRGen

TBD: Possibly link to a separate document explaining the architecture of IRGen.

## SILAnalysis and the PassManager

TBD: describe the mechanism by which passes invalidate and update the
PassManager and its avaiable analyses.

## High Level SIL Optimizations

HighLevelSILOptimizations.rst discusses how the optimizer imbues
certain special SIL types and SIL functions with higher level
semantics.

## Ownership SSA Form

### Derived Properties

1. Even though OSSA provides dataflow guarantees around SSA values, it does not
   provide any guarantees around objects being loaded/stored to memory in it of
   itself. When looking for memory errors, they are most likely to occur along
   boundaries of verified regions where values are accessed through memory.
2. OSSA guarantees that all ``ref_element_addr`` (i.e. fields) of a class typed
   SSA value are not used after the class typed SSA value is consumed. This
   ensures that use-after-free or dangling pointers due to fields can not occur
   from a ``ref_element_addr`` directly. This is because in the process of
   escaping the field must be copied, the compiler engineer can know in OSSA
   that all fields are always copied before escaping. This is a general property
   that we could use in other situations (for instance) with closures to start
   closures on the stack and copy them to the heap in a safe way.
3. Since all defs have a static lifetime semantics, PHIs (i.e. ``SILArguments``)
   must have an assigned ownership convention. This allows us to verify a
   program ignoring loops and will rigorously eliminate the program of
   understanding how reference counting operations flow around loops.
4. Previous algorithms around ownership in the Swift compiler have needed to
   consider both joint-dominance and joint-postdominance of sets of
   copies/destroys, the dominance property that comes with SSA causes the
   verifier to only need to prove that a set of instructions that take owned
   values joint-postdominate a single ``@owned`` producing def.
5. All "value projection" (e.g. ``struct_extract``) operations can only operate
   on ``@guaranteed`` values. This is because fundamentally these operations are
   giving access to a subvalue of a different ``@owned`` value
   non-destructively. Since we do not allow for a value to be partially destroy,
   this necessarily implies that such operations can only occur on
   ``@guaranteed`` values. To perform a destructive value projection, one must
   consume the entire value at once via a destructure operation
   (e.g. ``destructure_struct``). **NOTE** destructure operations inherently
   have multiple result values so require special handling in code.
6. There are a few instructions that are called "ownership forwarding
   instructions". These instructions are polymorphic over a set of
   ``ValueOwnershipKind`` and produce a value with the same ownership as one of
   its operands. Some examples of these type of instructions are casts,
   destructures, and enum routines.
7. There are only certain "ownership conversion" instructions that allow one to
   convert a value from one form of ownership to another. E.g.: ``copy_value``
   (convert a ``@guaranteed``, ``@unowned``, or ``@owned`` value to an
   ``@owned`` value), ``begin_borrow`` (convert an ``@owned`` or ``@guaranteed``
   value to a ``@guaranteed`` value).
8. Since there are no dataflow guarantees on trivial values, any trivial
   pointers that are formed from owned or guaranteed values must communicate the
   end of the use by calling ``fix_lifetime`` on the original object after the
   last use of the derived trivial value. Otherwise, the optimizer may attempt
   to shorten the life of the managed value to before the last unsafe use of the
   value resulting in a use-after-free. For example in the following::

```
     sil @foo : $@convention(thin) (@owned Class) -> () {
     bb0(%0 : $@owned Class)
       %1 = function_ref @get_raw_pointer : $@convention(thin) (@guaranteed Class) -> Builtin.RawPointer
       %2 = begin_borrow %0 : $Class
       %3 = apply %1(%2) : $@convention(thin) (@guaranteed Class) -> Builtin.RawPointer
       end_borrow %2 : $Class
       %4 = function_ref @use_raw_pointer : $@convention(thin) (Builtin.RawPointer) -> ()
       apply %4(%3) : $@convention(thin) (Builtin.RawPointer) -> ()
       destroy_value %0 : $Class
       ...
     }
```

   OSSA assumes that there is no connection in between ``%1`` and ``%3``, so it
   is legal for the optimizer to move the ``destroy_value`` of ``%0`` to before
   the call to ``use_raw_pointer``. To do this safely, one would use the
   ``fix_lifetime`` intrinsic::

```
     sil @foo : $@convention(thin) (@owned Class) -> () {
     bb0(%0 : $@owned Class)
       %1 = function_ref @get_raw_pointer : $@convention(thin) (@guaranteed Class) -> Builtin.RawPointer
       %2 = begin_borrow %0 : $Class
       %3 = apply %1(%2) : $@convention(thin) (@guaranteed Class) -> Builtin.RawPointer
       end_borrow %2 : $Class
       %4 = function_ref @use_raw_pointer : $@convention(thin) (Builtin.RawPointer) -> ()
       apply %4(%3) : $@convention(thin) (Builtin.RawPointer) -> ()
       fix_lifetime %0 : $Class
       destroy_value %0 : $Class
       ...
     }
```

   The ``fix_lifetime`` states that ``%0`` has some unknown reason that it must
   be live at this point. This would then tell the optimizer that it would not
   be safe to move the ``destroy_value`` past that point.
9. Since our dataflow conditions talk about paths to the end of the program,
   naturally we ignore proving dataflow constraints in regions that are
   unreachable or regions that are joint-postdominated by a set of
   unreachables. We still require that reachable defs produce compatible
   ValueOwnershipKind for unreachable uses.

### Ownership Model Checker

The ownership model checker has two different parts: the use checker and the
dataflow checker. The use checker runs over all def-use chains and validates
that all defs are paired with compatible uses and splits the uses into lifetime
ending and normal uses. The dataflow checker injests these sets and then perform
a bottom-up def-use traversal and proves that:

1. There is no path from a lifetime ending use to a non-lifetime ending use that
   does not visit the dominating ``SILValue`` again. This would be a use after
   free.
2. Along all paths through the program, we only visit a member of the consuming
   set exactly once. This would result in values being destroyed twice, i.e. use
   after free.
3. There does not exist a path through the program from the ``SILValue`` def
   that doesn't go through the consuming use set. This would result in a value
   being leaked.
