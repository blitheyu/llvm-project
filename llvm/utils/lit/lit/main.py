#!/usr/bin/env python

"""
lit - LLVM Integrated Tester.

See lit.pod for more information.
"""

from __future__ import absolute_import
import os
import platform
import random
import re
import sys
import time
import tempfile
import shutil
from xml.sax.saxutils import quoteattr

import lit.cl_arguments
import lit.discovery
import lit.display
import lit.LitConfig
import lit.run
import lit.Test
import lit.util

def main(builtinParameters = {}):
    # Create a temp directory inside the normal temp directory so that we can
    # try to avoid temporary test file leaks. The user can avoid this behavior
    # by setting LIT_PRESERVES_TMP in the environment, so they can easily use
    # their own temp directory to monitor temporary file leaks or handle them at
    # the buildbot level.
    lit_tmp = None
    if 'LIT_PRESERVES_TMP' not in os.environ:
        lit_tmp = tempfile.mkdtemp(prefix="lit_tmp_")
        os.environ.update({
                'TMPDIR': lit_tmp,
                'TMP': lit_tmp,
                'TEMP': lit_tmp,
                'TEMPDIR': lit_tmp,
                })
    # FIXME: If Python does not exit cleanly, this directory will not be cleaned
    # up. We should consider writing the lit pid into the temp directory,
    # scanning for stale temp directories, and deleting temp directories whose
    # lit process has died.
    try:
        main_with_tmp(builtinParameters)
    finally:
        if lit_tmp:
            try:
                shutil.rmtree(lit_tmp)
            except:
                # FIXME: Re-try after timeout on Windows.
                pass

def main_with_tmp(builtinParameters):
    opts = lit.cl_arguments.parse_args()

    if opts.show_version:
        print("lit %s" % (lit.__version__,))
        return

    userParams = create_user_parameters(builtinParameters, opts)

    # Decide what the requested maximum indvidual test time should be
    if opts.maxIndividualTestTime is not None:
        maxIndividualTestTime = opts.maxIndividualTestTime
    else:
        # Default is zero
        maxIndividualTestTime = 0

    isWindows = platform.system() == 'Windows'

    # Create the global config object.
    litConfig = lit.LitConfig.LitConfig(
        progname = os.path.basename(sys.argv[0]),
        path = opts.path,
        quiet = opts.quiet,
        useValgrind = opts.useValgrind,
        valgrindLeakCheck = opts.valgrindLeakCheck,
        valgrindArgs = opts.valgrindArgs,
        noExecute = opts.noExecute,
        debug = opts.debug,
        isWindows = isWindows,
        params = userParams,
        config_prefix = opts.configPrefix,
        maxIndividualTestTime = maxIndividualTestTime,
        maxFailures = opts.maxFailures,
        parallelism_groups = {},
        echo_all_commands = opts.echoAllCommands)

    # Perform test discovery.
    run = lit.run.Run(litConfig,
                      lit.discovery.find_tests_for_inputs(litConfig, opts.test_paths))

    # After test discovery the configuration might have changed
    # the maxIndividualTestTime. If we explicitly set this on the
    # command line then override what was set in the test configuration
    if opts.maxIndividualTestTime is not None:
        if opts.maxIndividualTestTime != litConfig.maxIndividualTestTime:
            litConfig.note(('The test suite configuration requested an individual'
                ' test timeout of {0} seconds but a timeout of {1} seconds was'
                ' requested on the command line. Forcing timeout to be {1}'
                ' seconds')
                .format(litConfig.maxIndividualTestTime,
                        opts.maxIndividualTestTime))
            litConfig.maxIndividualTestTime = opts.maxIndividualTestTime

    if opts.showSuites or opts.showTests:
        print_suites_or_tests(run, opts)
        return

    # Select and order the tests.
    numTotalTests = len(run.tests)

    if opts.filter:
        filter_tests(run, opts)

    order_tests(run, opts)

    # Then optionally restrict our attention to a shard of the tests.
    if (opts.numShards is not None) or (opts.runShard is not None):
        num_tests = len(run.tests)
        # Note: user views tests and shard numbers counting from 1.
        test_ixs = range(opts.runShard - 1, num_tests, opts.numShards)
        run.tests = [run.tests[i] for i in test_ixs]
        # Generate a preview of the first few test indices in the shard
        # to accompany the arithmetic expression, for clarity.
        preview_len = 3
        ix_preview = ", ".join([str(i+1) for i in test_ixs[:preview_len]])
        if len(test_ixs) > preview_len:
            ix_preview += ", ..."
        litConfig.note('Selecting shard %d/%d = size %d/%d = tests #(%d*k)+%d = [%s]' %
                       (opts.runShard, opts.numShards,
                        len(run.tests), num_tests,
                        opts.numShards, opts.runShard, ix_preview))

    # Finally limit the number of tests, if desired.
    if opts.maxTests is not None:
        run.tests = run.tests[:opts.maxTests]

    # Don't create more workers than tests.
    opts.numWorkers = min(len(run.tests), opts.numWorkers)

    increase_process_limit(litConfig, opts)

    display = lit.display.create_display(opts, len(run.tests),
                                         numTotalTests, opts.numWorkers)
    def progress_callback(test):
        display.update(test)
        if opts.incremental:
            update_incremental_cache(test)

    startTime = time.time()
    try:
        run.execute_tests(progress_callback, opts.numWorkers, opts.maxTime)
    except KeyboardInterrupt:
        sys.exit(2)
    testing_time = time.time() - startTime

    display.finish()

    if not opts.quiet:
        print('Testing Time: %.2fs' % (testing_time,))

    # Write out the test data, if requested.
    if opts.output_path is not None:
        write_test_results(run, litConfig, testing_time, opts.output_path)

    # List test results organized by kind.
    hasFailures = False
    byCode = {}
    for test in run.tests:
        if test.result.code not in byCode:
            byCode[test.result.code] = []
        byCode[test.result.code].append(test)
        if test.result.code.isFailure:
            hasFailures = True

    # Print each test in any of the failing groups.
    for title,code in (('Unexpected Passing Tests', lit.Test.XPASS),
                       ('Failing Tests', lit.Test.FAIL),
                       ('Unresolved Tests', lit.Test.UNRESOLVED),
                       ('Unsupported Tests', lit.Test.UNSUPPORTED),
                       ('Expected Failing Tests', lit.Test.XFAIL),
                       ('Timed Out Tests', lit.Test.TIMEOUT)):
        if (lit.Test.XFAIL == code and not opts.show_xfail) or \
           (lit.Test.UNSUPPORTED == code and not opts.show_unsupported) or \
           (lit.Test.UNRESOLVED == code and (opts.maxFailures is not None)):
            continue
        elts = byCode.get(code)
        if not elts:
            continue
        print('*'*20)
        print('%s (%d):' % (title, len(elts)))
        for test in elts:
            print('    %s' % test.getFullName())
        sys.stdout.write('\n')

    if opts.timeTests and run.tests:
        # Order by time.
        test_times = [(test.getFullName(), test.result.elapsed)
                      for test in run.tests]
        lit.util.printHistogram(test_times, title='Tests')

    for name,code in (('Expected Passes    ', lit.Test.PASS),
                      ('Passes With Retry  ', lit.Test.FLAKYPASS),
                      ('Expected Failures  ', lit.Test.XFAIL),
                      ('Unsupported Tests  ', lit.Test.UNSUPPORTED),
                      ('Unresolved Tests   ', lit.Test.UNRESOLVED),
                      ('Unexpected Passes  ', lit.Test.XPASS),
                      ('Unexpected Failures', lit.Test.FAIL),
                      ('Individual Timeouts', lit.Test.TIMEOUT)):
        if opts.quiet and not code.isFailure:
            continue
        N = len(byCode.get(code,[]))
        if N:
            print('  %s: %d' % (name,N))

    if opts.xunit_output_file:
        write_test_results_xunit(run, opts)

    # If we encountered any additional errors, exit abnormally.
    if litConfig.numErrors:
        sys.stderr.write('\n%d error(s), exiting.\n' % litConfig.numErrors)
        sys.exit(2)

    # Warn about warnings.
    if litConfig.numWarnings:
        sys.stderr.write('\n%d warning(s) in tests.\n' % litConfig.numWarnings)

    if hasFailures:
        sys.exit(1)
    sys.exit(0)


def create_user_parameters(builtinParameters, opts):
    userParams = dict(builtinParameters)
    for entry in opts.userParameters:
        if '=' not in entry:
            name,val = entry,''
        else:
            name,val = entry.split('=', 1)
        userParams[name] = val
    return userParams

def print_suites_or_tests(run, opts):
    # Aggregate the tests by suite.
    suitesAndTests = {}
    for result_test in run.tests:
        if result_test.suite not in suitesAndTests:
            suitesAndTests[result_test.suite] = []
        suitesAndTests[result_test.suite].append(result_test)
    suitesAndTests = list(suitesAndTests.items())
    suitesAndTests.sort(key = lambda item: item[0].name)

    # Show the suites, if requested.
    if opts.showSuites:
        print('-- Test Suites --')
        for ts,ts_tests in suitesAndTests:
            print('  %s - %d tests' %(ts.name, len(ts_tests)))
            print('    Source Root: %s' % ts.source_root)
            print('    Exec Root  : %s' % ts.exec_root)
            if ts.config.available_features:
                print('    Available Features : %s' % ' '.join(
                    sorted(ts.config.available_features)))

    # Show the tests, if requested.
    if opts.showTests:
        print('-- Available Tests --')
        for ts,ts_tests in suitesAndTests:
            ts_tests.sort(key = lambda test: test.path_in_suite)
            for test in ts_tests:
                print('  %s' % (test.getFullName(),))

    # Exit.
    sys.exit(0)

def filter_tests(run, opts):
    try:
        rex = re.compile(opts.filter)
    except:
        parser.error("invalid regular expression for --filter: %r" % (
                opts.filter))
    run.tests = [result_test for result_test in run.tests
                    if rex.search(result_test.getFullName())]

def order_tests(run, opts):
    if opts.shuffle:
        random.shuffle(run.tests)
    elif opts.incremental:
        run.tests.sort(key = by_mtime, reverse = True)
    else:
        run.tests.sort(key = lambda t: (not t.isEarlyTest(), t.getFullName()))

def by_mtime(test):
    fname = test.getFilePath()
    try:
        return os.path.getmtime(fname)
    except:
        return 0

def update_incremental_cache(test):
    if not test.result.code.isFailure:
        return
    fname = test.getFilePath()
    os.utime(fname, None)

def increase_process_limit(litConfig, opts):
    # Because some tests use threads internally, and at least on Linux each
    # of these threads counts toward the current process limit, try to
    # raise the (soft) process limit so that tests don't fail due to
    # resource exhaustion.
    try:
        cpus = lit.util.detectCPUs()
        desired_limit = opts.numWorkers * cpus * 2 # the 2 is a safety factor

        # Import the resource module here inside this try block because it
        # will likely fail on Windows.
        import resource

        max_procs_soft, max_procs_hard = resource.getrlimit(resource.RLIMIT_NPROC)
        desired_limit = min(desired_limit, max_procs_hard)

        if max_procs_soft < desired_limit:
            resource.setrlimit(resource.RLIMIT_NPROC, (desired_limit, max_procs_hard))
            litConfig.note('raised the process limit from %d to %d' % \
                               (max_procs_soft, desired_limit))
    except:
        pass

def write_test_results(run, lit_config, testing_time, output_path):
    try:
        import json
    except ImportError:
        lit_config.fatal('test output unsupported with Python 2.5')

    # Construct the data we will write.
    data = {}
    # Encode the current lit version as a schema version.
    data['__version__'] = lit.__versioninfo__
    data['elapsed'] = testing_time
    # FIXME: Record some information on the lit configuration used?
    # FIXME: Record information from the individual test suites?

    # Encode the tests.
    data['tests'] = tests_data = []
    for test in run.tests:
        test_data = {
            'name' : test.getFullName(),
            'code' : test.result.code.name,
            'output' : test.result.output,
            'elapsed' : test.result.elapsed }

        # Add test metrics, if present.
        if test.result.metrics:
            test_data['metrics'] = metrics_data = {}
            for key, value in test.result.metrics.items():
                metrics_data[key] = value.todata()

        # Report micro-tests separately, if present
        if test.result.microResults:
            for key, micro_test in test.result.microResults.items():
                # Expand parent test name with micro test name
                parent_name = test.getFullName()
                micro_full_name = parent_name + ':' + key

                micro_test_data = {
                    'name' : micro_full_name,
                    'code' : micro_test.code.name,
                    'output' : micro_test.output,
                    'elapsed' : micro_test.elapsed }
                if micro_test.metrics:
                    micro_test_data['metrics'] = micro_metrics_data = {}
                    for key, value in micro_test.metrics.items():
                        micro_metrics_data[key] = value.todata()

                tests_data.append(micro_test_data)

        tests_data.append(test_data)

    # Write the output.
    f = open(output_path, 'w')
    try:
        json.dump(data, f, indent=2, sort_keys=True)
        f.write('\n')
    finally:
        f.close()

def write_test_results_xunit(run, opts):
    # Collect the tests, indexed by test suite
    by_suite = {}
    for result_test in run.tests:
        suite = result_test.suite.config.name
        if suite not in by_suite:
            by_suite[suite] = {
                                'passes'   : 0,
                                'failures' : 0,
                                'skipped': 0,
                                'tests'    : [] }
        by_suite[suite]['tests'].append(result_test)
        if result_test.result.code.isFailure:
            by_suite[suite]['failures'] += 1
        elif result_test.result.code == lit.Test.UNSUPPORTED:
            by_suite[suite]['skipped'] += 1
        else:
            by_suite[suite]['passes'] += 1
    xunit_output_file = open(opts.xunit_output_file, "w")
    xunit_output_file.write("<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n")
    xunit_output_file.write("<testsuites>\n")
    for suite_name, suite in by_suite.items():
        safe_suite_name = quoteattr(suite_name.replace(".", "-"))
        xunit_output_file.write("<testsuite name=" + safe_suite_name)
        xunit_output_file.write(" tests=\"" + str(suite['passes'] +
            suite['failures'] + suite['skipped']) + "\"")
        xunit_output_file.write(" failures=\"" + str(suite['failures']) + "\"")
        xunit_output_file.write(" skipped=\"" + str(suite['skipped']) +
            "\">\n")

        for result_test in suite['tests']:
            result_test.writeJUnitXML(xunit_output_file)
            xunit_output_file.write("\n")
        xunit_output_file.write("</testsuite>\n")
    xunit_output_file.write("</testsuites>")
    xunit_output_file.close()

if __name__=='__main__':
    main()
