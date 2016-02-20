:orphan:

=================
The SIL ARC Model
=================

.. contents::

Abstract
========

This document attempts to formally describe the Automatic Reference Counting
(ARC) model used in the Swift Intermediate Language (SIL). The intention is for
this document to become the basis of a formal verifier that can be used to
verify the correctness of ARC based programs in SIL. The target audience for
this documentation are frontend developers who wish to target SIL or optimizer
developers who wish to implement correct optimizations that improve the
performance of ARC based programs. Before we get to formalities, we begin with
an informal introduction to ARC and some high level concepts that need to be
understood to work with ARC.

Inroduction and High Level Concepts
===================================

A Brief Introduction to ARC
---------------------------

ARC is a form of garbage collection where the programmer describes the ownership
relationships among the objects that make up the object graph of the
program. These ownership relations are specified by forcing all edges in the
object graph to have one of the following ownership types: **strong**,
**unowned**, or **unsafe**. These edges in terms of the language itself are
represented as references. A solution to the "ARC problem" is the insertion of
reference counting operations such that the lifetimes specified by the ownership
object graph are preserved. The current swift/clang frontend create a
conservative solution to this problem by inserting the appropriate
retain/release operation when ever a reference is created or destroyed. Then
optimizers run afterwards removing retains/releases which are redundant. An
example of this would be if one creates two references to the same object::

  A = ref(a)
  B = ref(a)
  ...
  destroy(A)
  destroy(B)

In this case, the retain/releases for A,B both guarantee the lifetime of A over
the same code region so are redundant. If an optimizer can recognize cases such
as this, the optimizer can remove the retains/releases associated with one of
A,B.

ARC at the SIL level
--------------------

At the SIL level, we necessarily take a lower level approach and base our model
on reasoning about the lifetime of memory instead of the lifetime of
objects. Thus at the SIL level, instead of speaking of an object graph, we speak
of a memory graph with nodes being memory allocations and ownership edges
consisting of references to these memory allocations. These references can be
represented as:

  1. Values stored in stack memory.
  2. Values stored in heap memory.
  3. Values stored in global memory.
  4. SSA values.

Just like at the language level, we use ``strong'', ``unowned'', and ``unsafe''
as the ownership types of our memory graph edges. A quick summary of the
guarantees that are provided by the various ownership specifiers are:

..

  +----------+-------------+--------------+
  | Ref Type | Destruction | Deallocation |
  +==========+=============+==============+
  | Strong   | Illegal     | Illegal      |
  +----------+-------------+--------------+
  | Unowned  | Allowed     | Illegal      |
  +----------+-------------+--------------+
  | Unsafe   | Allowed     | Allowed      |
  +----------+-------------+--------------+

In words:

1. A **Strong Reference** implies that the pointed to memory will not be
destroyed or deallocated for the entirety of the "validity" of the reference.

2. An **Unowned Reference** implies that the pointed to memory *may* be
destroyed, but that the memory will not be deallocated.

3. An **Unsafe Reference** implies that the pointed to memory *may* be destroyed
or deallocated at any point in time with the "validity" of the reference having
no affect on the objects lifetime.

Every time one uses one of these references, one creates a "use" in the object
graph that is endowed with lifetime guarantees of its use of the reference. A
solution to the "ARC problem" is the insertion of retains/releases such that the
lifetime guarantees of all uses in the program are preserved. The key thing to
take from this paragraph is that at the SIL level we do not care about the
precise lifetime of a piece of memory, rather we only care about the "lifetime"
specified by the union of the lifetime specification of the uses of the memories
references.

RC Operation Pairs and Locality
-------------------------------

At the SIL level, we never speak of retains/releases individually. Instead, we
always think in terms of pairs of RC operations. We can separate these groups of
reference counting pairs into two sorts of pairings based off of the locality of
the pairing relationship:

1. Function Local Pairs (let, var)
2. Global Pairs (globals, ivars)

This distinction is important since we can verify statically that function local
pairs are balanced properly. On the other hand, global retain/release balancing
can only be verified dynamically unless one is willing to perform expensive
whole program analysis (which may be impossible) and analyze all global usage
sites.

TODO: ADD EXAMPLES HERE OF WHY THIS IS USEFUL

Reasoning about Lifetimes
-------------------------

In order to maintain these relationships, the Swift compiler uses reference
counting. An eager solution to the ARC problem is constructed by the frontend
where a retain is inserted at each point such a reference is created. When ever
the reference is no longer valid, a release is inserted.

When is it safe to remove a copy? It is safe to remove a copy when you form a
transitive range! I.e.:

  retain
  release
  retain
  release

or:

  retain
  retain
  release
  release



a copy converts a +0 reference to a +1 reference.

Certain things /only/ take +1 references.

Other things preserve reference relationships. Everything is specified at the
language level. Each chain of retains/releases should be correct by only
reasoning on the reference counting SSA chain locally.

Then you take the SSA chain and modify the copy modifiers. Then you never need
to move around code. You just modify the copy modifiers. A later pass can then
just go through and lower the copies. Depending on their modifiers, they insert
retain/release/whatever.

After that point, retain/releases are never moved.

The SSA chain specifies the behavior. Any SSA chain must start with a +1. +1s
propagate through. We just go up and turn off unnecessary ones.

A release is a conversion from a +1 -> +0.

reasons about the inter-relationships among his objects by
specifying relationships among the objects. The frontend of the compiler then
conservatively, eagerly inserts operations to ensure that the relationships
among the objects in question.

This is a nice high level way of looking at the issue in question, but from
SIL's perspective it is lacking. Instead in SIL, we think about lifetimes of
memory, not objects.

At any place in the CFG where I have a region where a value is used

PHI nodes have to have ownership specifiers.

Does SILGen ever have merge points of paths with different ownership specifiers?
We have to be able to guarantee that.

I want to be able to from a single reference to a value, traverse the SSA
use-def list and discover everything I need to know to do ARC optimization.

* We are leaving weak/unowned as an implementation detail.

Memory Locations and Lifetime
-----------------------------

Formal Description
==================

Define a program 
