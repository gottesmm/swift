# swift_build_support/products/clang.py -------------------------*- python -*-
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ----------------------------------------------------------------------------

import os

from . import cmark
from . import foundation
from . import libcxx
from . import libdispatch
from . import libicu
from . import llvm
from . import product
from . import cmake_product
from . import swift
from . import xctest


class LLBuild(cmake_product.CMakeProduct):
    def __init__(self, *args, **kwargs):
        super(cmake_product.CMakeProduct, self).__init__(*args, **kwargs)

    @classmethod
    def is_build_script_impl_product(cls):
        """is_build_script_impl_product -> bool

        Whether this product is produced by build-script-impl.
        """
        return False

    @classmethod
    def is_before_build_script_impl_product(cls):
        """is_before_build_script_impl_product -> bool

        Whether this product is build before any build-script-impl products.
        """
        return False

    @classmethod
    def get_dependencies(cls):
        return [cmark.CMark,
                llvm.LLVM,
                libcxx.LibCXX,
                libicu.LibICU,
                swift.Swift,
                libdispatch.LibDispatch,
                foundation.Foundation,
                xctest.XCTest]

    def should_build(self, host_target):
        return self.args.build_llbuild

    def should_clean(self, host_target):
        return False

    def should_test(self, host_target):
        return self.args.test_llbuild

    def should_install(self, host_target):
        return self.args.install_llbuild

    def get_custom_cmake_flags(self, host_target):
        build_root = os.path.dirname(self.build_dir)

        llvm_build_dir = os.path.join(
            build_root,
            llvm.LLVM.get_build_dir_name(host_target))
        filecheck_path = os.path.join(llvm_build_dir, 'bin', 'FileCheck')

        foundation_build_dir = os.path.join(
            build_root,
            foundation.Foundation.get_build_dir_name(host_target))
        foundation_cmake_dir = os.path.join(foundation_build_dir,
                                            'cmake', 'modules')
        dispatch_build_dir = os.path.join(
            build_root,
            libdispatch.LibDispatch.get_build_dir_name(host_target))
        dispatch_cmake_dir = os.path.join(dispatch_build_dir, 'cmake',
                                          'modules')
        dispatch_source_dir = os.path.abspath(
            os.path.join(self.source_dir, '..', 'libdispatch'))

        lit_path = os.path.abspath(
            os.path.join(self.source_dir, '..', 'llvm', 'utils', 'lit', 'lit.py'))

        extra_flags = {
            'LLBUILD_SUPPORT_BINDINGS' : 'Swift',
            'FILECHECK_EXECUTABLE:PATH' : filecheck_path,
            'LIBDISPATCH_BUILD_DIR' : dispatch_build_dir,
            'dispatch_DIR' : dispatch_cmake_dir,
            'Foundation_DIR' : foundation_cmake_dir,
            'LIBDISPATCH_SOURCE_DIR' : dispatch_source_dir,
            'LIT_EXECUTABLE' : lit_path,
            'LLBUILD_ENABLE_ASSERTIONS' : self.args.llbuild_assertions,
            }

        return extra_flags

    def get_test_targets(self):
        # The target's name is 'test'
        return ['test']

    def get_install_targets(self):
        return ['install-swift-build-tool', 'install-libllbuildSwift']

    @classmethod
    def build_with_just_built_toolchain(cls):
        return False
