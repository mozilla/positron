# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at http://mozilla.org/MPL/2.0/.

# This commit parser is used by the legacy kind; once that is gone, it can be
# removed.

import argparse
import copy
import re
import shlex

TRY_DELIMITER = 'try:'
TEST_CHUNK_SUFFIX = re.compile('(.*)-([0-9]+)$')

# The build type aliases are very cryptic and only used in try flags these are
# mappings from the single char alias to a longer more recognizable form.
BUILD_TYPE_ALIASES = {
    'o': 'opt',
    'd': 'debug'
}

def parse_test_opts(input_str):
    '''Test argument parsing is surprisingly complicated with the "restrictions"
    logic this function is responsible for parsing this out into a easier to
    work with structure like { test: '..', platforms: ['..'] }'''

    # Final results which we will return.
    tests = []

    cur_test = {}
    token = ''
    in_platforms = False

    def add_test(value):
        cur_test['test'] = value.strip()
        tests.insert(0, cur_test)

    def add_platform(value):
        # Ensure platforms exists...
        cur_test['platforms'] = cur_test.get('platforms', [])
        cur_test['platforms'].insert(0, value.strip())

    # This might be somewhat confusing but we parse the string _backwards_ so
    # there is no ambiguity over what state we are in.
    for char in reversed(input_str):
        # , indicates exiting a state
        if char == ',':
            # Exit a particular platform.
            if in_platforms:
                add_platform(token)

            # Exit a particular test.
            else:
                add_test(token)
                cur_test = {}

            # Token must always be reset after we exit a state
            token = ''
        elif char == '[':
            # Exiting platform state entering test state.
            add_platform(token)
            token = ''
            in_platforms = False
        elif char == ']':
            # Entering platform state.
            in_platforms = True
        else:
            # Accumulator.
            token = char + token

    # Handle any left over tokens.
    if token:
        add_test(token)

    return tests


def escape_whitespace_in_brackets(input_str):
    '''
    In tests you may restrict them by platform [] inside of the brackets
    whitespace may occur this is typically invalid shell syntax so we escape it
    with backslash sequences    .
    '''
    result = ""
    in_brackets = False
    for char in input_str:
        if char == '[':
            in_brackets = True
            result += char
            continue

        if char == ']':
            in_brackets = False
            result += char
            continue

        if char == ' ' and in_brackets:
            result += '\ '
            continue

        result += char

    return result


def normalize_platform_list(alias, all_builds, build_list):
    if build_list == 'all':
        return all_builds
    return [alias.get(build, build) for build in build_list.split(',')]

def normalize_test_list(aliases, all_tests, job_list):
    '''
    Normalize a set of jobs (builds or tests) there are three common cases:

        - job_list is == 'none' (meaning an empty list)
        - job_list is == 'all' (meaning use the list of jobs for that job type)
        - job_list is comma delimited string which needs to be split

    :param dict aliases: Alias mapping for jobs...
    :param list all_tests: test flags from job_flags.yml structure.
    :param str job_list: see above examples.
    :returns: List of jobs
    '''

    # Empty job list case...
    if job_list is None or job_list == 'none':
        return []

    tests = parse_test_opts(job_list)

    if not tests:
        return []

    # Special case where tests is 'all' and must be expanded
    if tests[0]['test'] == 'all':
        results = []
        all_entry = tests[0]
        for test in all_tests:
            entry = { 'test': test }
            # If there are platform restrictions copy them across the list.
            if 'platforms' in all_entry:
                entry['platforms'] = list(all_entry['platforms'])
            results.append(entry)
        return parse_test_chunks(aliases, all_tests, results)
    else:
        return parse_test_chunks(aliases, all_tests, tests)


def handle_alias(test, aliases, all_tests):
    '''
    Expand a test if its name refers to an alias, returning a list of test
    dictionaries cloned from the first (to maintain any metadata).

    :param dict test: the test to expand
    :param dict aliases: Dict of alias name -> real name.
    :param list all_tests: test flags from job_flags.yml structure.
    '''
    if test['test'] not in aliases:
        return [test]

    alias = aliases[test['test']]
    def mktest(name):
        newtest = copy.deepcopy(test)
        newtest['test'] = name
        return newtest

    def exprmatch(alias):
        if not alias.startswith('/') or not alias.endswith('/'):
            return [alias]
        regexp = re.compile('^' + alias[1:-1] + '$')
        return [t for t in all_tests if regexp.match(t)]

    if isinstance(alias, str):
        return [mktest(t) for t in exprmatch(alias)]
    elif isinstance(alias, list):
        names = sum([exprmatch(a) for a in alias], [])
        return [mktest(t) for t in set(names)]
    else:
        return [test]


def parse_test_chunks(aliases, all_tests, tests):
    '''
    Test flags may include parameters to narrow down the number of chunks in a
    given push. We don't model 1 chunk = 1 job in taskcluster so we must check
    each test flag to see if it is actually specifying a chunk.

    :param dict aliases: Dict of alias name -> real name.
    :param list all_tests: test flags from job_flags.yml structure.
    :param list tests: Result from normalize_test_list
    :returns: List of jobs
    '''
    results = []
    seen_chunks = {}
    for test in tests:
        matches = TEST_CHUNK_SUFFIX.match(test['test'])

        if not matches:
            results.extend(handle_alias(test, aliases, all_tests))
            continue

        name = matches.group(1)
        chunk = int(matches.group(2))
        test['test'] = name

        for test in handle_alias(test, aliases, all_tests):
            name = test['test']
            if name in seen_chunks:
                seen_chunks[name].add(chunk)
            else:
                seen_chunks[name] = {chunk}
                test['test'] = name
                test['only_chunks'] = seen_chunks[name]
                results.append(test)

    # uniquify the results over the test names
    results = {test['test']: test for test in results}.values()
    return results

def extract_tests_from_platform(test_jobs, build_platform, build_task, tests):
    '''
    Build the list of tests from the current build.

    :param dict test_jobs: Entire list of tests (from job_flags.yml).
    :param dict build_platform: Current build platform.
    :param str build_task: Build task path.
    :param list tests: Test flags.
    :return: List of tasks (ex: [{ task: 'test_task.yml' }]
    '''
    if tests is None:
        return []

    results = []

    for test_entry in tests:
        if test_entry['test'] not in test_jobs:
            continue

        test_job = test_jobs[test_entry['test']]

        # Verify that this job can actually be run on this build task...
        if 'allowed_build_tasks' in test_job and build_task not in test_job['allowed_build_tasks']:
            continue

        if 'platforms' in test_entry:
            # The default here is _exclusive_ rather then inclusive so if the
            # build platform does not specify what platform(s) it belongs to
            # then we must skip it.
            if 'platforms' not in build_platform:
                continue

            # Sorta hack to see if the two lists intersect at all if they do not
            # then we must skip this set.
            common_platforms = set(test_entry['platforms']) & set(build_platform['platforms'])
            if not common_platforms:
                # Tests should not run on this platform...
                continue

        # Add the job to the list and ensure to copy it so we don't accidentally
        # mutate the state of the test job in the future...
        specific_test_job = copy.deepcopy(test_job)

        # Update the task configuration for all tests in the matrix...
        for build_name in specific_test_job:
            # NOTE: build_name is always "allowed_build_tasks"
            for test_task_name in specific_test_job[build_name]:
                # NOTE: test_task_name is always "task"
                test_task = specific_test_job[build_name][test_task_name]
                test_task['unittest_try_name'] = test_entry['test']
                # Copy over the chunk restrictions if given...
                if 'only_chunks' in test_entry:
                    test_task['only_chunks'] = \
                            copy.copy(test_entry['only_chunks'])

        results.append(specific_test_job)

    return results

'''
This module exists to deal with parsing the options flags that try uses. We do
not try to build a graph or anything here but match up build flags to tasks via
the "jobs" datastructure (see job_flags.yml)
'''

def parse_commit(message, jobs):
    '''
    :param message: Commit message that is typical to a try push.
    :param jobs: Dict (see job_flags.yml)
    '''

    # shlex used to ensure we split correctly when giving values to argparse.
    parts = shlex.split(escape_whitespace_in_brackets(message))
    try_idx = None
    for idx, part in enumerate(parts):
        if part == TRY_DELIMITER:
            try_idx = idx
            break

    if try_idx is None:
        return [], 0

    # Argument parser based on try flag flags
    parser = argparse.ArgumentParser()
    parser.add_argument('-b', '--build', dest='build_types')
    parser.add_argument('-p', '--platform', nargs='?', dest='platforms', const='all', default='all')
    parser.add_argument('-u', '--unittests', nargs='?', dest='tests', const='all', default='all')
    parser.add_argument('-i', '--interactive', dest='interactive', action='store_true', default=False)
    parser.add_argument('-j', '--job', dest='jobs', action='append')
    # In order to run test jobs multiple times
    parser.add_argument('--trigger-tests', dest='trigger_tests', type=int, default=1)
    # Once bug 1250993 is fixed we can only use --trigger-tests
    parser.add_argument('--rebuild', dest='trigger_tests', type=int, default=1)
    args, unknown = parser.parse_known_args(parts[try_idx:])

    # Normalize default value to something easier to detect.
    if args.jobs == ['all']:
        args.jobs = None

    # Expand commas.
    if args.jobs:
        expanded = []
        for job in args.jobs:
            expanded.extend(j.strip() for j in job.split(','))
        args.jobs = expanded

    # Then builds...
    if args.build_types is None:
        args.build_types = []

    build_types = [ BUILD_TYPE_ALIASES.get(build_type, build_type) for
            build_type in args.build_types ]

    aliases = jobs['flags'].get('aliases', {})

    platforms = set()
    for base in normalize_platform_list(aliases, jobs['flags']['builds'], args.platforms):
        # Silently skip unknown platforms.
        if base not in jobs['builds']:
            continue
        platforms.add(base)
        platforms.update(jobs['builds'][base].get('extra-builds', []))

    tests = normalize_test_list(aliases, jobs['flags']['tests'], args.tests)

    result = []

    # Expand the matrix of things!
    for platform in platforms:
        platform_builds = jobs['builds'][platform]

        for build_type in build_types:
            # Not all platforms have debug builds, etc...
            if build_type not in platform_builds['types']:
                continue

            platform_build = platform_builds['types'][build_type]
            build_task = platform_build['task']

            additional_parameters = platform_build.get('additional-parameters', {})

            # Generate list of post build tasks that run on this build
            post_build_jobs = []
            for job_flag in jobs['flags'].get('post-build', []):
                job = jobs['post-build'][job_flag]
                if ('allowed_build_tasks' in job and
                        build_task not in job['allowed_build_tasks']):
                    continue
                job = copy.deepcopy(job)
                job['job_flag'] = job_flag
                post_build_jobs.append(job)

            # Node for this particular build type
            result.append({
                'task': build_task,
                'post-build': post_build_jobs,
                'dependents': extract_tests_from_platform(
                    jobs['tests'], platform_builds, build_task, tests
                ),
                'additional-parameters': additional_parameters,
                'build_name': platform,
                'build_type': build_type,
                'interactive': args.interactive,
                'when': platform_builds.get('when', {}),
            })

    # Process miscellaneous tasks.

    def filtertask(name, task):
        # args.jobs == None implies all tasks.
        if args.jobs is None:
            return True

        if name in args.jobs:
            return True

        for tag in task.get('tags', []):
            if tag in args.jobs:
                return True

        return False

    for name, task in sorted(jobs.get('tasks', {}).items()):
        if not filtertask(name, task):
            continue

        # TODO support tasks that are defined as dependent on another one.
        if not task.get('root', False):
            continue

        result.append({
            'task': task['task'],
            'post-build': [],
            'dependents': [],
            'additional-parameters': task.get('additional-parameters', {}),
            'build_name': name,
            # TODO support declaring a different build type
            'build_type': name,
            'is_job': True,
            'interactive': args.interactive,
            'when': task.get('when', {})
        })

    # Times that test jobs will be scheduled
    trigger_tests = args.trigger_tests

    return result, trigger_tests
