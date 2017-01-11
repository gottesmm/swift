
import md5
import os
import sys

import swift_tools

import list_reducer
from list_reducer import TESTRESULT_KEEPPREFIX
from list_reducer import TESTRESULT_KEEPSUFFIX
from list_reducer import TESTRESULT_NOFAILURE


class ReduceMiscompilingFunctions(list_reducer.ListReducer):

    def __init__(self, functions, silextract, tester):
        list_reducer.ListReducer.__init__(self, functions)
        self.silextract = silextract
        self.tester = tester

    def run_test(self, prefix, suffix):
        if len(suffix) > 0 and self._test_funclist(suffix):
            return (TESTRESULT_KEEPSUFFIX, prefix, suffix)
        if len(prefix) > 0 and self._test_funclist(prefix):
            return (TESTRESULT_KEEPPREFIX, prefix, suffix)
        return (TESTRESULT_NOFAILURE, prefix, suffix)

    # Split functions in a module into two groups: Those that are under
    # consideration for miscompilation vs those that are not. Eventually, we
    # will split the module into separate partial modules and then compile so
    # we can find miscompiles. For now, we just look for crashsers in the
    # optimizer.
    def _test_funclist(self, funcs):
        funclist_hash = self.get_funclist_hash(funcs)
        funclist_path = os.path.join(self.silextract.config.work_dir,
                                     'func_list_' + funclist_hash)
        with open(funclist_path, 'w') as funclist_file:
            for f in funcs:
                funclist_file.write(f + '\n')

        print("Checking to see if the program is misoptimized with func "
              "list: %s" % funclist_path)

        # Split the module into the two halves of the program.
        (outfile_stem, outfile_invert_stem) = \
            self.get_output_filename_stems(funclist_hash)
        outfile = self.silextract.get_suffixed_filename(outfile_stem)
        outfile_invert = \
            self.silextract.get_suffixed_filename(outfile_invert_stem)
        self.silextract.invoke_with_functions(funclist_path, outfile)
        self.silextract.invoke_with_functions(funclist_path, outfile_invert,
                                              invert=True)
        return self.tester(outfile, outfile_stem, outfile_invert,
                           outfile_invert_stem)

    def get_output_filename_stems(self, funclist_hash):
        filename_stem = funclist_hash
        return (filename_stem, filename_stem + '_invert')

    def get_funclist_hash(self, funcs):
        m = md5.new()
        for f in funcs:
            m.update(f)
        return m.hexdigest()

def function_bug_reducer(input_file, nm, sil_opt_invoker, sil_extract_invoker,
                         pass_list, tester):
    functions = [s[1] for s in nm.get_symbols(input_file) if s[0] == 'F']

    print("Base case crashes! Trying to reduce *.sib file")

    # Otherwise, reduce the list of pases that cause the optimzier to crash.
    r = ReduceMiscompilingFunctions(functions, sil_extract_invoker,
                                    tester)
    if not r.reduce_list():
        print("Failed to find miscompiling pass list!")
    cmdline = tester.cmdline_with_passlist(pass_list)
    print("*** Successfully Reduced file!")
    print("*** Final File: %s" % tester.result)
    print("*** Final Functions: %s" % (' '.join(r.target_list)))
    print("*** Repro command line: %s" % (' '.join(cmdline)))


def invoke_function_bug_reducer(args):
    """Given a path to a sib file with canonical sil, attempt to find a perturbed
list of function given a specific pass that causes the perf pipeline to crash
    """
    tools = swift_tools.SwiftTools(args.swift_build_dir)
    config = swift_tools.SILToolInvokerConfig(args)
    nm = swift_tools.SILNMInvoker(config, tools)

    input_file = args.input_file
    extra_args = args.extra_args
    sil_opt_invoker = swift_tools.SILOptInvoker(config, tools,
                                                      input_file,
                                                      extra_args)

    # Make sure that the base case /does/ crash.
    filename = sil_opt_invoker.get_suffixed_filename('base_case')
    result = sil_opt_invoker.invoke_with_passlist(args.pass_list, filename)
    # If we succeed, there is no further work to do.
    if result['exit_code'] == 0:
        print("Success with PassList: %s" % (' '.join(args.pass_list)))
        return

    sil_extract_invoker = swift_tools.SILFuncExtractorInvoker(config,
                                                              tools,
                                                              input_file)

    function_bug_reducer(input_file, nm, sil_opt_invoker, sil_extract_invoker,
                         args.pass_list, OptimizerTester(sil_opt_invoker, pass_list))


def add_parser_arguments(parser):
    """Add parser arguments for func_bug_reducer"""
    parser.set_defaults(func=invoke_function_bug_reducer)
    parser.add_argument('input_file', help='The input file to optimize')
    parser.add_argument('--module-cache', help='The module cache to use')
    parser.add_argument('--sdk', help='The sdk to pass to sil-func-extractor')
    parser.add_argument('--target', help='The target to pass to '
                        'sil-func-extractor')
    parser.add_argument('--resource-dir',
                        help='The resource-dir to pass to sil-func-extractor')
    parser.add_argument('--work-dir',
                        help='Working directory to use for temp files',
                        default='bug_reducer')
    parser.add_argument('--module-name',
                        help='The name of the module we are optimizing')
    parser.add_argument('--pass', help='pass to test', dest='pass_list',
                        action='append')
    parser.add_argument('--extra-silopt-arg', help='extra argument to pass to '
                        'sil-opt',
                        dest='extra_args', action='append')
