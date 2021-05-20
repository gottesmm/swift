# swift_build_support/products/cmake_product.py -------------------*- python -*-
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ------------------------------------------------------------------------------

import abc
import os
import platform

import product
from .. import cmake
from .. import targets
from .. import shell


def join_path(*paths):
    return os.path.abspath(os.path.join(*paths))


class CMakeProduct(product.Product):
    """A product that builds/tests/installs in a standard way via cmake."""

    def __init__(self, *args, **kwargs):
        super(product.Product, self).__init__(*args, **kwargs)

    @classmethod
    def build_with_just_built_toolchain(cls):
        """Return true if this supposed to build with the just built compiler or false
        if it is supposed to be built with the host compiler.
        """
        # Force our users to specify this.
        raise NotImplementedError

    def clean(self, host_target):
        """clean() -> void

        Perform the clean, for a non-build-script-impl product.
        """
        delete_cmd = ['rm', '-rf']
        if self.args.verbose_build:
            delete_cmd.append('-v')
        delete_cmd.append(self.build_dir)
        shell.call(delete_cmd)

    def get_toolchain_path(self):
        return targets.toolchain_path(self.args.install_destdir,
                                      self.args.install_prefix)

    def get_toolchain_tool(self, tool_name):
        return join_path(self.get_toolchain_path(), 'bin', tool_name)

    def get_custom_cmake_flags(self, host_target):
        """Callback that must be implemented by subclasses. Must at least return an empty list.
        Will be passed the current host_target being built.
        """
        raise NotImplementedError

    def build(self, host_target):
        """build() -> void

        Perform the build, for a non-build-script-impl product.
        """
        install_destdir = self.args.install_destdir
        build_root = os.path.dirname(self.build_dir)
        toolchain_path = targets.toolchain_path(install_destdir,
                                                self.args.install_prefix)
        cmake_toolchain_file_path = os.path.join(
            self.build_dir, 'Toolchain.cmake')

        cmake_toolchain_file = {
            'CMAKE_SYSTEM_NAME': platform.system(),
            'CMAKE_C_COMPILER_TARGET': 'arm64-apple-macosx11.0',
            'CMAKE_CXX_COMPILER_TARGET': 'arm64-apple-macosx11.0',
            'CMAKE_Swift_COMPILER_TARGET': 'arm64-apple-macosx11.0',
            'CMAKE_AR' : self.toolchain.ar,
            'CMAKE_LIBTOOL' : self.toolchain.libtool,
            }
        # Then setup the compilers setting the appropriate compiler toolchain.
        if self.__class__.build_with_just_built_toolchain():
            cmake_toolchain_file.update({
                'CMAKE_C_COMPILER' : self.get_toolchain_tool('clang'),
                'CMAKE_CXX_COMPILER' : self.get_toolchain_tool('clang++'),
                'CMAKE_Swift_COMPILER' : self.get_toolchain_tool('swiftc'),
                })
        else:
            cmake_toolchain_file.update({
                'CMAKE_C_COMPILER' : self.toolchain.cc,
                'CMAKE_CXX_COMPILER' : self.toolchain.cxx,
                'CMAKE_Swift_COMPILER' : self.toolchain.swiftc,
                })

        # Set specific language flags here. We hard wire the module-cache to a
        # build dir so that they each have their own module cache's preventing
        # weird bugs.
        cache_path = '-module-cache-path {}'.format(os.path.join(self.build_dir,
                                                                 'module-cache'))
        cmake_toolchain_file.update({
            'CMAKE_Swift_FLAGS' : cache_path,
            })

        # Finally grab any cmake flags from our child
        cmake_toolchain_file.update(self.get_custom_cmake_flags(host_target))

        # Then create our toolchain file.
        toolchain_file = ""
        for x, y in sorted(cmake_toolchain_file.items()):
            toolchain_file += "set({} \"{}\")\n".format(x,y)
        if True:#self.args.verbose_build:
            print("Toolchain File Begin: {}".format(cmake_toolchain_file_path))
            print(toolchain_file)
            print("Toolchain File Complete.")
        with open(cmake_toolchain_file_path, 'w') as f:
            f.write(toolchain_file)

        # Then configure.
        # Default setup for all cmake projects.
        cmake_configure_command = [
            self.toolchain.cmake,
            '-G', 'Ninja',
            '-S', self.source_dir,
            '-B', self.build_dir,
            '-DCMAKE_TOOLCHAIN_FILE={}'.format(cmake_toolchain_file_path),
            '-DCMAKE_BUILD_TYPE:STRING={}'.format(self.args.build_variant),
            '-DCMAKE_INSTALL_PREFIX:PATH={}'.format(self.args.install_prefix),
        ]
        shell.call(cmake_configure_command)

        # Then run the build_command.
        cmake_build_command = [
            self.toolchain.cmake,
            '--build', self.build_dir,
            '--',
            ]
        if self.args.verbose_build:
            cmake_build_command.append('-v')
        shell.call(cmake_build_command)

    def get_test_targets(self):
        """Call back that returns a list of cmake targets to run to test this product. Must be implemented by subclasses or will assert"""
        raise NotImplementedError

    def test(self, host_target):
        """test() -> void

        Run the tests, for a cmake non-build-script-impl product. We rely on the
        user to implement this since we do not use cmake's testing facilities.
        """
        test_targets = self.get_test_targets()

        test_cmd = [
            self.toolchain.cmake,
            '--build', self.build_dir,
            '--',
            ]
        test_cmd.extend(test_targets)
        shell.call(test_cmd)

    def get_install_targets(self):
        """Call back that returns a list of cmake targets to run to install this product. Must be implemented by subclasses or will assert"""
        raise NotImplementedError

    def install(self, host_target):
        """install() -> void

        Install to the toolchain, for a non-build-script-impl product.
        """
        cmake_install_command = [
            self.toolchain.cmake,
            '--build', self.build_dir,
            '--',
            ]
        cmake_install_command.extend(self.get_install_targets())
        shell.call(cmake_install_command,
                   env={'DESTDIR': self.args.install_destdir})


