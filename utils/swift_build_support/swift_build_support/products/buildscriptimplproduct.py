# swift_build_support/products/buildscriptimplproduct.py --------*- python -*-
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

from . import product
from . import cmark

# A super class of all build-script-impl based products. Needed to express a
# class type based dependency on all build-script-impl products in our
# scheduling dag. We require all such jobs to all have the same incoming
# dependency since they must all be configured, built, installed, tested
# together at once.
class BuildScriptImplProduct(product.Product):

    def __init__(self, *args, **kwargs):
        product.Product.__init__(self, *args, **kwargs)

    @classmethod
    def is_build_script_impl_product(self):
        return True

    @classmethod
    def get_dependencies(cls):
        """get_dependencies() -> [typeof(Product)]

        Returns a list of Product objects that are dependencies of this class.
        """
        return [cmark.CMark]
