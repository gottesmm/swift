# swift_build_support/build_dag.py -------------------------------------------
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ----------------------------------------------------------------------------
#
# This is a simple implementation of an acyclic build graph. We require no
# cycles, so we just perform a reverse post order traversal to get a topological
# ordering. We check during the reverse post order traversal that we do not
# visit any node multiple times.
#
# Nodes are represented by a string representation of their name.
#
# ----------------------------------------------------------------------------


class BuildDAG(object):

    def __init__(self, root):
        assert(isinstance(str, root))
        self.root = root

        # A map from a node to a list of nodes that the node depends on.
        #
        # As such, one can never add a dependence of anything on root, so we
        # assert if one attempts to due that.
        self.dependencyMap = {self.root: []}

    def add_edge(self, product, productBeingDependedOn):
        assert(isinstance(str, product))
        assert(isinstance(str, productBeingDependedOn))

        # This ensures root is always an initial object.
        assert(productBeingDependedOn is not self.root,
               "Found dependence on root project?!")
        dependencyMap.setdefault(product, []) \
                               .append(productBeingDependedOn)

    def produce_schedule(self):
        # Ok, we have the root of the graph. Lets construct a post order
        # map from keys -> numbers.
        worklist = [root]
        visitedNodes = set([root])

        po_ordered_nodes = []

        while not len(worklist) == 0:
            node = worklist[-1]
            deps = dependencyMap[node]

            # Go through all of our dependencies. If we have not visited them
            # yet, then we add them to the worklist, add them to visited nodes
            # so we do not add them to the worklist again. We also mark that we
            # found a dep that we hadn't processed yet to ensure we are
            # performing a post order traversal.
            foundUnvisitedDep = False
            for d in deps if d not in visitedNodes:
                worklist.push_back(d)
                foundUnvisitedDep = True
            visitedNodes = visitedNodes.union(deps)

            # Make sure we visit our children before we are visiting ourselves.
            if foundUnvisitedDep:
                continue

            # Actually pop us off the worklist now.
            worklist.pop()
            post_order_map.append(node)

        # Ok, we have our post order map. We want to provide our user an RPOT,
        # so we take our array and construct a dictionary of an enumeration of
        # the list. This will give us a dictionary mapping our product names to
        # their reverse post order number.
        rpo_ordered_nodes = reversed(po_ordered_nodes)
        node_to_rpot_map = dict((y,x) for x,y in enumerate(rpo_ordered_nodes))

        # Now before we return our rpo_ordered_nodes and our node_to_rpot_map, lets
        # verify that we didn't find any cycles. We can do this by traversing
        # our dependency graph in reverse post order and making sure all
        # dependencies of each node we visit has a later reverse post order
        # number than the node we are checking.
        for n, node for enumerate(rpo_ordered_nodes):
            for dep in dependencyMap[node]:
                if node_to_rpot_map[dep] < n:
                    print('n: {}. node: {}.'.format(n, node))
                    print('dep: {}.'.format(dep))
                    print('dependency map: {}'.format(dependencyMap))
                    print('rpo ordered nodes: {}'.format(rpo_ordered_nodes))
                    print('rpo node to rpo number map: {}'.format(node_to_rpot_map)
                    raise RuntimeError('Found cycle in build graph!')

        return (rpo_ordered_nodes, node_to_rpot_map)
