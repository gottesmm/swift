
# Semantic ARC

This is a proposal for a series of changes to the SIL IR in order to improve the ability for compiler writers/optimizers to verify and optimize the ARC operations. Traditionally, ARC has been optimized via a bidirectional dataflow algorithm that attempts to prove that a set of retains and releases joint dominate/post-dominate each other. If the appropriate conditions are discovered during the dataflow then the retain/release sets are removed. Even though this algorithm is very powerful and removes most redundant retain/release pairings is does have several weaknesses:

1. It attempts to prove via various heuristics that applying ARC operations to two retainable values will manipulate the same reference count. This is not always easy to prove and in the face of a changing IR the lack of verification causes this to be brittle. In addition, if the heuristics are incorrect, then retain and release operations that do not always manipulate the same reference count. This would then result in a use after free.

2. 

### Reference Count Identity Problem

### Pairing Problems

## Solving the Reference Count Identity Problem

### Add RC Identity semantics to use-def chains

### Create an RC Identity Verifier

## Solving the Pairing Problem

### Transition to copy_value

### Add Ownership Semantics to use-def chains

### Create an Ownership Semantic Verifier
