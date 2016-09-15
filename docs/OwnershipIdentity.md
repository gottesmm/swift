
Ownership Identity
==================

A core Ownership concept in SIL is the concept of `Ownership Identity` and
instructions that preserve `Ownership Identity`. In this section, we:

1. Define the concepts of Ownership Identity and Ownership Identity Roots.
2. Contrast Ownership Identity Analysis with alias analysis.
3. Extend Ownership Identity Roots to Ownership Identity Root Sets to handle the
   general aggregate case.
4. Discuss instructions/properties that cause certain instructions which "seem"
   to be Ownership Identical to not be so.

Definitions
-----------

In SIL, every 

Let ``I`` be a SIL instruction with n operands and m results. We say that ``I``
is a (i, j) Ownership Identity preserving instruction if performing a
``retain_value`` on the ith SSA argument immediately before ``I`` is executed is
equivalent to performing a ``retain_value`` on the jth SSA result of ``I``
immediately following the execution of ``I``. For example in the following, if::

    retain_value %x
    %y = unary_instruction %x

is equivalent to::

    %y = unary_instruction %x
    retain_value %y

then we say that unary_instruction is a (0,0) RC Identity preserving
instruction. In a case of a unary instruction, we omit (0,0) and just say that
the instruction is RC Identity preserving.

TODO: This section defines RC identity only for loadable types. We also need to
define it for instructions on addresses and instructions that mix addresses and
values. It should be pretty straight forward to do this.

Given two SSA values ``%a``, ``%b``, we define ``%a`` as immediately RC
identical to ``%b`` (or ``%a ~rci(i,j) %b``) if there exists an instruction
``I`` such that:

- ``%a`` is the jth result of ``I``.
- ``%b`` is the ith argument of ``I``.
- ``I`` is (i, j) RC identity preserving.

Due to the nature of SSA form, we can not even speak of symmetry or
reflexivity. But we do get transitivity! Easily if ``%b ~rci %a`` and ``%c ~rci
%b``, we must by these two assumptions be able to do the following::

  retain_value %a
  %b = unary_instruction %a
  %c = unary_instruction %b

which by our assumption means that we can perform the following code motion::

  %b = unary_instruction %a
  %c = unary_instruction %b
  retain_value %c

our desired result. But we would really like for this operation to be reflexive
and symmetric. To get around this issue, we define the equivalent relation RC
identity as follows: We say that ``%a ~rc %b`` if:

1. ``%a == %b``
2. ``%a ~rci %b`` or ``%b ~rci %a``.
3. There exists a finite sequence of ``n`` SSA values ``{%a[i]}`` such that:
   a. ``%a ~rci %a[0]``
   b. ``%a[i] ~rci %a[i+1]`` for all ``i < n``.
   c. ``%a[n] ~rci %b``.

These equivalence classes consisting of chains of RC identical values are
computed via the SILAnalysis called ``RC Identity Analysis``. By performing ARC
optimization on RC Identical operations, our optimizations are able to operate
on the level of granularity that we actually care about, ignoring superficial
changes in SSA form that still yield manipulations of the same reference count.

.. admonition:: NOTE

   RCIdentityAnalysis is a flow insensitive analysis. Dataflow that needs to
   be flow sensitive must handle phi nodes in the dataflow itself.

Contrasts with Alias Analysis
-----------------------------

A common question is what is the difference in between RC Identity analysis and
alias analysis. While alias analysis is attempting to determine if two memory
location are the same, RC identity analysis is attempting to determine if
reference counting operations on different values would result in the same
"ownership state" (i.e. reference count) being modified.

Some interesting examples of where RC identity differs from alias analysis are:

 - ``struct`` is an RC identity preserving operation if the ``struct`` literal
   only has one non-trivial operand. This means for instance that any struct
   with one reference counted field used as an owning pointer is RC Identical
   with its owning pointer (a useful property for Arrays).

 - An ``enum`` instruction is always RC Identical with the given tuple payload.

 - A ``tuple`` instruction is an RC identity preserving operation if the
   ``tuple`` literal has one non-trivial operand.

 - ``init_class_existential`` is an RC identity preserving operation since
   performing a retain_value on a class existential is equivalent to performing
   a retain_value on the class itself.

The corresponding value projection operations have analogous properties.

.. admonition:: NOTE

    An important consequence of RC Identity is that value types with only one
    RCIdentity are a simple case for ARC optimization to handle. The ARC
    optimizer relies on other optimizations like SROA, Function Signature Opts,
    and SimplifyCFG (for block arguments) to try and eliminate cases where value
    types have multiple reference counted subtypes. If one has a struct type
    with multiple reference counted sub fields, wrapping the struct in a COW
    data structure (for instance storing the struct in an array of one element)
    will reduce the reference count overhead.

what is ``retain_value`` and why is it important
------------------------------------------------

Notice in the section above how we defined RC identity using the SIL
``retain_value`` instruction. ``retain_value`` and ``release_value`` are the
catch-all please retain or please release this value at the SIL level. The
following table is a quick summary of what ``retain_value`` (``release_value``)
does when applied to various types of objects:

+-----------+--------------+-------------------------------------------------------------------------------------+
| Ownership | Type         | Effect                                                                              |
+===========+==============+=====================================================================================+
| Strong    | Class        | Increment strong ref count of class                                                 |
+-----------+--------------+-------------------------------------------------------------------------------------+
| Any       | Struct/Tuple | retain_value each field                                                             |
+-----------+--------------+-------------------------------------------------------------------------------------+
| Any       | Enum         | switch on the enum and apply retain_value to the enum case's payload (if it exists) |
+-----------+--------------+-------------------------------------------------------------------------------------+
| Unowned   | Class        | Increment the unowned ref count of class                                            |
+-----------+--------------+-------------------------------------------------------------------------------------+

.. admonition:: Notice

  Aggregate value types like struct/tuple/enums's definitions are defined
  recursively via retain_value on payloads/fields. This is why operations like
  ``struct_extract`` do not always propagate RC identity.

Conversions
-----------

Conversions are a common operation that propagate RC identity. But not all
conversions have these properties. In this section, we attempt to explain why
this is true. The rule for conversions is that a conversion that preserves RC
identity must have the following properties:

1. Both of its arguments must be non-trivial values with the same ownership
   semantics (i.e. unowned, strong, weak). This means that conversions such as:

   - address_to_pointer
   - pointer_to_address
   - unchecked_trivial_bitcast
   - ref_to_raw_pointer
   - raw_pointer_to_ref
   - ref_to_unowned
   - unowned_to_ref
   - ref_to_unmanaged
   - unmanaged_to_ref

   The reason why we want the ownership semantics to be the same is that
   whenever there is a change in ownership semantics, we want the programmer to
   explicitly reason about the change in ownership semantics.

2. The instruction must not introduce type aliasing. This disqualifies such
   casts as:

   - unchecked_addr_cast
   - unchecked_bitwise_cast

This means in sum that conversions that preserve types and preserve
non-trivialness are the interesting instructions.

ARC and Enums
-------------

Enum types provide interesting challenges for ARC optimization. This is because
if there exists one case where an enum is non-trivial, the aggregate type in all
situations must be treated as if it is non-trivial. An important consideration
here is that when performing ARC optimization on cases, one has to be very
careful about ensuring that one only ignores reference count operations on
values that are able to be proved to be that specific case.

.. admonition:: TODO

  This section needs to be filled out more.
