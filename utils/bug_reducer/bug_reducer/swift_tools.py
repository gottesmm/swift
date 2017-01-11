
import os
import subprocess

import subprocess_utils

DRY_RUN = False
SQUELCH_STDERR = False
ECHO_CALLS = True


def br_call(args, dry_run=DRY_RUN, echo=ECHO_CALLS):
    if dry_run or echo:
        print('BRCALL: ' + ' '.join(args))
    if dry_run:
        return {'exit_code': 0, 'fingerprint': ''}
    return subprocess_utils.call_fingerprint(args, echo_stderr=True,
                                             echo_stdout=True)


# We use this since our squelching of stderr can hide missing file errors.
def sanity_check_file_exists(f):
    if not os.access(f, os.F_OK):
        raise RuntimeError('Error! Could not find file: ' + f)


class SwiftTools(object):
    """A utility class that enables users to easily find sil-tools without needing
to constantly reform paths to the build directory. Also provides safety by
asserting if one of the tools does not exist at the specified path"""

    def __init__(self, swift_build_dir):
        self.swift_build_dir = swift_build_dir

    def _get_tool(self, name):
        path = os.path.join(self.swift_build_dir, 'bin', name)
        if not os.access(path, os.F_OK):
            error_msg = "Error! {} does not exist at: {}".format(name, path)
            raise RuntimeError(error_msg)
        return path

    @property
    def sil_nm(self):
        """Return the path to sil-nm in the specified swift build directory. Throws a
runtime error if the tool does not exist"""
        return self._get_tool('sil-nm')

    @property
    def swiftc(self):
        """Return the path to swiftc in the specified swift build directory. Throws a
runtime error if the tool does not exist"""
        return self._get_tool('swiftc')

    @property
    def sil_opt(self):
        """Return the path to sil-opt in the specified swift build directory. Throws a
runtime error if the tool does not exist"""
        return self._get_tool('sil-opt')

    @property
    def sil_llvm_gen(self):
        """Return the path to sil-llvm-gen in the specified swift build directory. Throws a runtime error if the tool does not exist"""
        return self._get_tool('sil-llvm-gen')

    @property
    def sil_func_extractor(self):
        """Return the path to sil-func-extractor in the specified swift build
directory. Throws a runtime error if the tool does not exist."""
        return self._get_tool('sil-func-extractor')

    @property
    def sil_passpipeline_dumper(self):
        """Return the path to sil-passpipeline-dumper in the specified swift build
directory. Throws a runtime error if the tool does not exist

        """
        return self._get_tool('sil-passpipeline-dumper')


def maybe_abspath(x):
    if x is None:
        return x
    return os.path.abspath(x)


class SILToolInvokerConfig(object):

    def __init__(self, args):
        self.module_cache = args.module_cache
        self.sdk = args.sdk
        self.target = args.target
        self.resource_dir = maybe_abspath(args.resource_dir)
        self.work_dir = maybe_abspath(args.work_dir)
        self.module_name = args.module_name


class SILToolInvoker(object):

    def __init__(self, config, extra_args=None):
        self.config = config
        self.extra_args = extra_args or []

    @property
    def arg_seperator(self):
        return "="

    def convert_arg_to_cmdline(self, argname, argvalue):
        return ["%s=%s" % (argname, argvalue)]

    def base_args(self, emit_sib=False, emit_obj=False):
        x = [self.tool]
        if self.config.sdk is not None:
            x.extend(self.convert_arg_to_cmdline('-sdk', self.config.sdk))
        if self.config.target is not None:
            x.extend(self.convert_arg_to_cmdline('-target', self.config.target))
        if self.config.resource_dir is not None:
            x.extend(self.convert_arg_to_cmdline('-resource-dir', self.config.resource_dir))
        if self.config.module_cache is not None:
            x.extend(self.convert_arg_to_cmdline('-module-cache-path', self.config.module_cache))
        if self.config.module_name is not None:
            x.extend(self.convert_arg_to_cmdline('-module-name', self.config.module_name))
        if emit_sib:
            x.append("-emit-sib")
        return x

    @property
    def tool(self):
        raise RuntimeError('Abstract Method')


class SILConstantInputToolInvoker(SILToolInvoker):

    def __init__(self, config, tools, initial_input_file, extra_args):
        SILToolInvoker.__init__(self, config, extra_args)
        self.tools = tools

        # Start by creating our workdir if necessary
        subprocess.check_call(["mkdir", "-p", self.config.work_dir])

        # Then copy our input file into the work dir
        base_input_file = os.path.basename(initial_input_file)
        (base, ext) = os.path.splitext(base_input_file)
        self.base_input_file_stem = base
        self.original_base_input_file_ext = ext
        self.base_input_file_ext = ".sib"

        # First emit an initial *.sib file. This ensures no matter if we have a
        # *.swiftmodule, *.sil, or *.sib file, we are always using *.sib.
        self.input_file = initial_input_file
        sanity_check_file_exists(initial_input_file)

    def _invoke(self, *args, **kwargs):
        raise RuntimeError('Abstract method')

    def get_suffixed_filename(self, suffix, ext=None):
        if ext is None:
            ext = self.base_input_file_ext
        basename = self.base_input_file_stem + '_' + suffix
        basename += ext
        return os.path.join(self.config.work_dir, basename)


class SILOptInvoker(SILConstantInputToolInvoker):

    def __init__(self, config, tools, input_file, extra_args):
        SILConstantInputToolInvoker.__init__(self, config, tools, input_file,
                                             extra_args)
        self.input_file = self.get_suffixed_filename('initial')
        self._invoke(input_file, [], True, self.input_file)

    @property
    def tool(self):
        return self.tools.sil_opt

    def _cmdline(self, input_file, passes, emit_sib, output_file='-'):
        assert(isinstance(emit_sib, bool))
        assert(isinstance(output_file, str))
        base_args = self.base_args(emit_sib=emit_sib, emit_obj=False)
        sanity_check_file_exists(input_file)
        base_args.extend([input_file, '-o', output_file])
        base_args.extend(self.extra_args)
        base_args.extend(passes)
        return base_args

    def _invoke(self, input_file, passes, emit_sib, output_filename):
        cmdline = self._cmdline(input_file, passes, emit_sib, output_filename)
        return br_call(cmdline)

    def invoke_with_passlist(self, passes, output_filename):
        return self._invoke(self.input_file, passes, True, output_filename)

    def cmdline_with_passlist(self, passes):
        return self._cmdline(self.input_file, passes, False)


class SILFuncExtractorInvoker(SILConstantInputToolInvoker):

    def __init__(self, config, tools, input_file):
        SILConstantInputToolInvoker.__init__(self, config, tools, input_file,
                                             [])

    @property
    def tool(self):
        return self.tools.sil_func_extractor

    def _cmdline(self, input_file, funclist_path, emit_sib, output_file='-',
                 invert=False):
        assert(isinstance(emit_sib, bool))
        assert(isinstance(output_file, str))

        sanity_check_file_exists(input_file)
        sanity_check_file_exists(funclist_path)
        assert(isinstance(funclist_path, str))
        base_args = self.base_args(emit_sib=emit_sib, emit_obj=False)
        base_args.extend([input_file, '-o', output_file,
                          '-func-file=%s' % funclist_path])
        if invert:
            base_args.append('-invert')
        return base_args

    def _invoke(self, input_file, funclist_path, output_filename,
                invert=False):
        assert(isinstance(funclist_path, str))
        cmdline = self._cmdline(input_file, funclist_path, True, output_filename,
                                invert)
        return br_call(cmdline)

    def invoke_with_functions(self, funclist_path, output_filename,
                              invert=False):
        assert(isinstance(funclist_path, str))
        return self._invoke(self.input_file, funclist_path, output_filename,
                            invert)


class SILNMInvoker(SILToolInvoker):

    def __init__(self, config, tools):
        self.tools = tools
        SILToolInvoker.__init__(self, config)

    @property
    def tool(self):
        return self.tools.sil_nm

    def get_symbols(self, input_file):
        sanity_check_file_exists(input_file)
        cmdline = self.base_args(emit_sib=False, emit_obj=False)
        cmdline.append(input_file)
        output = subprocess.check_output(cmdline)
        for l in output.split("\n")[:-1]:
            t = tuple(l.split(" "))
            assert(len(t) == 2)
            yield t


class SwiftLLVMGenInvoker(SILConstantInputToolInvoker):

    def __init__(self, config, tools, input_file, extra_args):
        SILConstantInputToolInvoker.__init__(self, config, tools, input_file,
                                             extra_args)

    @property
    def tool(self):
        return self.tools.sil_llvm_gen

    def _cmdline(self, input_file, output_file=None):
        base_args = self.base_args(emit_sib=False, emit_obj=True)
        sanity_check_file_exists(input_file)
        base_args.extend([input_file])
        if output_file is not None:
            assert(isinstance(output_file, str))
            base_args.extend(['-o', output_file])
        base_args.extend(['-output-kind=object'])
        base_args.extend(self.extra_args)
        return base_args

    def _invoke(self, input_file, output_filename):
        cmdline = self._cmdline(input_file, output_filename)
        return br_call(cmdline)

    def invoke(self, output_filename):
        return self._invoke(self.input_file, output_filename)

    def cmdline_with_passlist(self, passes):
        return self._cmdline(self.input_file, passes)

class SwiftOptCompileInvoker(object):

    def __init__(self, config, tools, input_file, extra_args, run_script):
        self.config = config
        self.tools = tools
        self.extra_args = extra_args
        self.run_script = run_script
        self.sil_opt_invoker = SILOptInvoker(config, tools, input_file, extra_args)

    def invoke_with_passlist(self, passes, output_sib_filename, output_obj_filename):
        result = self.sil_opt_invoker.invoke_with_passlist(passes, output_sib_filename)
        if result['exit_code'] != 0:
            raise RuntimeError('Optimizer Compile Time Crasher?!')
        result = SwiftLLVMGenInvoker(self.config, self.tools, output_sib_filename, self.extra_args).invoke(output_obj_filename)
        print result
        if result['exit_code'] != 0:
            raise RuntimeError('Swift LLVM Gen Compile Time Crasher?!')
        return br_call([self.run_script, output_obj_filename])

    def cmdline_with_passlist(self, passes):
        return self.sil_opt_invoker.cmdline_with_passlist(passes)

    @property
    def input_file(self):
        return self.sil_opt_invoker.input_file

    @input_file.setter
    def input_file(self, new_input_file):
        self.sil_opt_invoker.input_file = new_input_file

    def get_suffixed_filename(self, *args, **kwargs):
        return self.sil_opt_invoker.get_suffixed_filename(*args, **kwargs)
