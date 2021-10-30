import os
import re
import sys
import glob
import tempfile
import subprocess

from ast import parse
from optparse import OptionParser

#------------------------------------------------------------------------------
# MIT License
#
# Copyright (c) 2021 Bob Hood
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to
# deal in the Software without restriction, including without limitation the
# rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
# sell copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.
#------------------------------------------------------------------------------

class LockInfo:
    def __init__(self, name = "", id = "", deferred = False, source = None):
        if source is None:
            self.mutex_name = name
            self.id_name = id  # !empty if mutex name is invalid as variable
            self.is_deferred = deferred
            self.is_locked = False if deferred else True
            self.outer_scope = False # does this lock exist outside the current scope?
            self.excluded = False # do not instrument this mutex instance
        else:
            # do copy construction
            self.mutex_name = source.mutex_name
            self.id_name = source.id_name
            self.is_deferred = source.is_deferred
            self.is_locked = source.is_locked
            self.outer_scope = source.outer_scope
            self.excluded = source.excluded

    def _info(self):
        return 'mutex: {}, id: {}, locked: {}, deferred: {}, outer_scope: {}, excluded: {}'.format(self.mutex_name, self.id_name, self.is_locked, self.is_deferred, self.outer_scope, self.excluded)

    def __str__(self):
        return self._info()

    def __repr__(self):
        return self._info()

def map_scopes(filename, lines, options):
    if options.sanitize_input:
        # run 'filename' through clang-format before processing
        command = [options.clangformat, filename]
        # we let subprocess throw an unhandled exception, halting
        # execution, if 'clangformat' isn't correct
        output = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT).communicate()[0] #.decode("utf-8")

        handle, filename = tempfile.mkstemp()
        with os.fdopen(handle, "wb") as f:
            f.write(output)

    in_string = False
    in_single_comment = False
    in_multi_comment = False
    in_escape = 0
    last_char = ''
    string_char = ''
    line_no = 0
    col_no = 0

    line = []

    scopes_tree = []
    scope_stack = []

    file_data = ''
    with open(filename) as f:
        file_data = f.read()

    for c in file_data:
        if c == '\n':
            line_no += 1
            col_no = 0

            lines.append(''.join(line))
            line = []
            in_single_comment = False
        else:
            if c == '\\':
                if in_string:
                    in_escape = 2 if last_char != '\\' else 0
            elif c == '/':
                if last_char == '/':
                    if (not in_single_comment) and (not in_multi_comment) and (not in_string):
                        if in_escape == 0:
                            in_single_comment = True
                elif last_char == '*':
                    if in_multi_comment and (not in_string):
                        in_multi_comment = False
            elif c == '*':
                if last_char == '/':
                    if (not in_single_comment) and (not in_multi_comment) and (not in_string):
                        in_multi_comment = True
            elif (c == '"') or (c == "'"):
                if (not in_single_comment) and (not in_multi_comment):
                    if in_escape == 0:
                        if in_string:
                            if string_char == c:
                                in_string = False
                        else:
                            string_char = c
                            in_string = True
            elif c == '{':
                if (not in_single_comment) and (not in_multi_comment):
                    if not in_string:
                        if in_escape == 0:
                            scope_stack.append([[line_no, col_no], [0, 0], []])
            elif c == '}':
                if (not in_single_comment) and (not in_multi_comment):
                    if (not in_string) and len(scope_stack) != 0:
                        if in_escape == 0:
                            # roll this scope back up to its parent (if applicable)
                            scope_token = scope_stack[-1]
                            scope_stack = scope_stack[:-1]

                            scope_token[1][0] = line_no
                            scope_token[1][1] = col_no

                            if len(scope_stack) == 0:
                                scopes_tree.append(scope_token)
                            else:
                                scope_stack[-1][2].append(scope_token)
            line.append(c)
            col_no += 1

            if in_escape != 0:
                in_escape -= 1

        last_char = c

    if len(scopes_tree) == 0:
        return []

    # create a mirror of scopes that matches the input file line-for-line
    max_lines = len(lines)
    scopes = [[] for i in range(max_lines)]

    # recursively process all nested scope starts/stops, maintaining parenting
    def process_children(child_list):
        start_line, start_col = child_list[0]
        end_line, end_col = child_list[1]
        if start_line == end_line:
            # this scope starts/ends on the same line
            scopes[start_line].append((start_col, end_col))
        else:
            # a start column with a negative end indicates a scope start
            scopes[start_line].append((start_col, -1))
            # an end column with a negative start indicates a scope end
            scopes[end_line].append((-1, end_col))

        for child in child_list[2]:
            process_children(child)

    for scope in scopes_tree:
        process_children(scope)

    if options.sanitize_input:
        os.remove(filename)

    return scopes

def is_valid_variable_name(name):
    try:
        parse('{} = None'.format(name))
    except:
        return False

    return True

def instrument(lines, scopes, options):
    # were lines changed?
    changed = False

    # new lines of the file
    new_lines = []

    # regular epxressions to identify areas of interest
    unique_lock_re = r'std::unique_lock<.+>\s+(\w+)\((.+)\)'
    lock_re = r'(\w+)\s*\.\s*lock\s*\(\s*\)'
    unlock_re = r'(\w+)\s*\.\s*unlock\s*\(\s*\)'
    local_include_re = r'\s*#include\s*"' # no capture
    system_include_re = r'\s*#include\s*<' # no capture

    # keep track of the last local and system includes we've seen
    last_local_include = -1
    last_system_include = -1

    # each new scope we enter gets a copy of the current scope
    scope_level = 0
    scope_stack = []
    indent_stack = ['']

    for line_ndx, line in enumerate(lines):

        # adjust scopes before processing the line
        for scope in scopes[line_ndx]:
            # scope on same line?
            if scope[0] != -1 and scope[1] != -1:
                pass  # we don't currently support processing within this

            # scope starts?
            elif scope[0] != -1:
                # add/duplicate current stack
                if len(scope_stack):
                    new_map = {}
                    for key in scope_stack[-1]:
                        info = scope_stack[-1][key]
                        # copy construct the new entry
                        new_map[key] = LockInfo(source=info)
                        # flag its origin
                        new_map[key].outer_scope = True

                    scope_stack.append(new_map)
                else:
                    scope_stack.append({})
                indent_stack.append('')
                scope_level += 1

            # scope ends?
            elif scope[1] != -1:
                # for any mutexes in the current scope that are currently
                # locked, inject an explicit unlock
                indent = options.indent * scope_level
                if options.align and (len(indent_stack[scope_level]) != 0):
                    indent = indent_stack[scope_level]

                for key in scope_stack[-1]:
                    info = scope_stack[-1][key]
                    if not info.excluded:
                        if not info.outer_scope:
                            comment_str = "  // aids Dreadlock's bookkeeping {}".format(scope_level)
                            if info.is_locked:
                                unlock_str = ''
                                if len(info.id_name):
                                    unlock_str = "%sDREADLOCK_UNLOCK_ID(%s, %s);" % (indent, info.mutex_name, info.id_name)
                                else:
                                    unlock_str = "%sDREADLOCK_UNLOCK(%s);" % (indent, info.mutex_name)

                                if 'return' in new_lines[-1]:
                                    new_lines.insert(-1, "{}{}".format(unlock_str, comment_str))
                                else:
                                    new_lines.append("{}{}".format(unlock_str, comment_str))

                            destr_str = ''
                            if len(info.id_name):
                                destr_str = "%sDREADLOCK_DESTRUCT_ID(%s, %s);" % (indent, info.mutex_name, info.id_name)
                            else:
                                destr_str = "%sDREADLOCK_DESTRUCT(%s);" % (indent, info.mutex_name)

                            if 'return' in new_lines[-1]:
                                new_lines.insert(-1, "{}{}".format(destr_str, comment_str))
                            else:
                                new_lines.append("{}{}".format(destr_str, comment_str))

                            changed = True

                # discard current stack
                scope_stack = scope_stack[:-1]
                indent_stack = indent_stack[:-1]
                scope_level -= 1

        result = re.search(local_include_re, line)
        if result != None:
            last_local_include = line_ndx

        result = re.search(system_include_re, line)
        if result != None:
            last_system_include = line_ndx

        # capture the indent of the first line encountered within the scope
        # in case we are directed to align
        if (len(scopes[line_ndx]) == 0) and (len(line) != 0) and (len(indent_stack[scope_level]) == 0):
            indent = []
            for i in range(len(line)):
                if not line[i].isspace():
                    break
                indent.append(line[i])
            indent_stack[scope_level] = ''.join(indent)

        # look for and parse std::unique_lock declarations
        ndx = line.find('std::unique_lock')
        if ndx != -1:
            result = re.search(unique_lock_re, line)
            if result != None:
                lock_name = result.group(1)
                mutex_name = result.group(2)
                is_deferred = 'std::defer_lock' in mutex_name
                if ',' in mutex_name:
                    mutex_name = mutex_name.split(',')[0]

                excluded = ('DREADLOCK' in line) # don't re-instrument!
                excluded = excluded or (mutex_name in options.excludes)

                id_name = ""

                # is 'mutex_name' a valid identifier?
                if not is_valid_variable_name(mutex_name):
                    items = []

                    if '->' in mutex_name:
                        items = mutex_name.split('->')
                    elif '.' in mutex_name:
                        items = mutex_name.split('.')
                    else:
                        assert False, "Cannot find delimiter in mutex name '%s'" % mutex_name

                    id_name = items[-1]

                if not excluded:
                    original = line[ndx:]

                    revert_str = ''
                    if not options.disable_revert:
                        revert_str = " // {{%s%s}}" % (line[:ndx], original)

                    if len(id_name):
                        if is_deferred:
                            line = line[:ndx] + ("DREADLOCK_DEFER_ID(%s, %s);%s" % (mutex_name, id_name, revert_str))
                        else:
                            line = line[:ndx] + ("DREADLOCK_ID(%s, %s);%s" % (mutex_name, id_name, revert_str))
                    else:
                        if is_deferred:
                            line = line[:ndx] + ("DREADLOCK_DEFER(%s);%s" % (mutex_name, revert_str))
                        else:
                            line = line[:ndx] + ("DREADLOCK(%s);%s" % (mutex_name, revert_str))

                    changed = True

                scope_stack[-1][lock_name] = LockInfo(name=mutex_name, id=id_name, deferred=is_deferred)
                info = scope_stack[-1][lock_name]
                info.excluded = excluded

            new_lines.append(line)

        # is an existing unique_lock instance being locked?
        elif '.lock()' in line:
            result = re.search(lock_re, line)
            if result != None:
                lock_name = result.group(1)
                if lock_name in scope_stack[-1]:
                    ndx = line.find('.lock()')
                    ndx -= len(lock_name)

                    info = scope_stack[-1][lock_name]

                    if not info.excluded:
                        info.is_locked = True

                        original = line[ndx:]

                        revert_str = ''
                        if not options.disable_revert:
                            revert_str = " // {{%s%s}}" % (line[:ndx], original)

                        if len(info.id_name):
                            line = line[:ndx] + ("DREADLOCK_LOCK_ID(%s, %s);%s" % (info.mutex_name, info.id_name, revert_str))
                        else:
                            line = line[:ndx] + ("DREADLOCK_LOCK(%s);%s" % (info.mutex_name, revert_str))

                        changed = True

            new_lines.append(line)

        # is an existing unique_lock instance being unlocked?
        elif '.unlock()' in line:
            result = re.search(unlock_re, line)
            if result != None:
                lock_name = result.group(1)
                if lock_name in scope_stack[-1]:
                    ndx = line.find('.unlock()')
                    ndx -= len(lock_name)

                    info = scope_stack[-1][lock_name]

                    if not info.excluded:
                        info.is_locked = False

                        original = line[ndx:]

                        revert_str = ''
                        if not options.disable_revert:
                            revert_str = " // {{%s%s}}" % (line[:ndx], original)

                        if len(info.id_name):
                            line = line[:ndx] + ("DREADLOCK_UNLOCK_ID(%s, %s);%s" % (info.mutex_name, info.id_name, revert_str))
                        else:
                            line = line[:ndx] + ("DREADLOCK_UNLOCK(%s);%s" % (info.mutex_name, revert_str))

                        changed = True

            new_lines.append(line)

        else:
            new_lines.append(line)

    if changed:
        # insert our Dreadlock.h header into the module

        if last_local_include != -1:
            new_lines.insert(last_local_include + 1, '#include "Dreadlock.h"')
        elif last_system_include != -1:
            new_lines.insert(last_system_include + 1, '#include "Dreadlock.h"')
        else:
            # insert it as the first line in the module
            new_lines.insert(0, '#include "Dreadlock.h"')

    return new_lines, changed

if __name__ == "__main__":
    print('Dreadlock Instrument -- infuse C++ modules with dread of mutex deadlocks.')
    print('by Bob Hood\n')

    parser = OptionParser()

    parser.add_option("-i", "--indent", action="store", dest="indent", default='    ', help="Specify the indent value to use for each scope level.")
    parser.add_option("-a", "--apply", action="store_true", dest="apply", default=True, help="Instrument each module to use Dreadlock.")
    parser.add_option("-r", "--revert", action="store_true", dest="revert", default=False, help="Reverse the Dreadlock instrumentation of one or more modules.")
    parser.add_option("-R", "--disable-revert", action="store_true", dest="disable_revert", default=False, help="Instrument modules without the ability to revert changes.")
    parser.add_option("-o", "--overwrite", action="store_true", dest="overwrite", default=False, help="Overwrite the original file with the result.")
    parser.add_option("-d", "--debug", action="store_true", dest="debug", default=False, help="Display enhanced diagnostic info to the console.")
    parser.add_option("-s", "--sanitize", action="store_true", dest="sanitize_input", default=False, help="Run the module through clang-format before processing.")
    parser.add_option("-x", "--exclude", action="append", dest="excludes", default=[], help="Exclude specific mutex declarations or modules from Dreadlock instrumentation.")
    parser.add_option("-A", "--align", action="store_true", dest="align", default=False, help="Align additions to previous indents, if possible; use scope level otherwise.")
    parser.add_option("-D", "--dry-run", action="store_true", dest="dry_run", default=False, help="Perform all processing, but do not generate output.")
    #parser.add_option("-D", "--destruct", action="store_true", dest="destruct", default=False, help="Add Dreadlock destruct tracking for each instrumented mutex.")
    (options, args) = parser.parse_args()

    if options.revert:
        options.apply = False

    options.indent = options.indent.replace("\\t", '\t')
    options.indent = options.indent.replace("<tab>", '\t')

    if options.sanitize_input:
        if 'CLANG_FORMAT_EXE' in os.environ:
            options.clangformat = os.environ['CLANG_FORMAT_EXE']
        else:
            if os.name == 'nt':
                options.clangformat = 'clang-format.exe'
            else:
                options.clangformat = 'clang-format'

    new_excludes = []
    for exclude in options.excludes:
        if os.path.exists(exclude):
            new_excludes += [mutex.rstrip() for mutex in open(exclude).readlines()]
        else:
            new_excludes.append(exclude)
    options.excludes = new_excludes

    modules = []
    for arg in args:
        files = glob.glob(arg)
        if len(files):
            modules += glob.glob(arg)
        else:
            modules.append(arg)

    if len(args) == 0 or len(modules) == 0:
        print("No files specified!  Nothing to do!", file=sys.stderr)
        sys.exit(1)

    for module in modules:
        assert os.path.exists(module), "File '%s' does not exist!" % module

        bypass_module = False

        module_upper = module.casefold()
        for exclude in options.excludes:
            exclude_upper = exclude.casefold()
            if module_upper == exclude_upper:
                bypass_module = True
                break

        if bypass_module:
            print("Excluding file '%s'." % module)
            continue

        if options.apply:
            file_lines = []
            scopes = map_scopes(module, file_lines, options)
            if options.debug:
                for i in range(len(scopes)):
                    s = "{}: {}".format(i, scopes[i])
                    s += ' ' * (20 - len(s))
                    if len(scopes[i]):
                        s += '!> {}'.format(file_lines[i])
                    else:
                        s += '-> {}'.format(file_lines[i])
                    print(s)
                sys.exit(0)

            if options.overwrite:
                print("Instrumenting '%s' ..." % module, end='')
                sys.stdout.flush()

            new_lines, changed = instrument(file_lines, scopes, options)
            if changed:
                if not options.dry_run:
                    if not options.overwrite:
                        print('\n'.join(new_lines))
                    else:
                        with open(module, 'w') as f:
                            f.write('\n'.join(new_lines))

                            # always terminate a file with a newline.
                            # the universe will thank you...
                            if not new_lines[-1].endswith('\n'):
                                f.write('\n')

                print(" done.")
            else:
                print(" file was not modified.")

        elif options.revert:
            dreadlock_include_re = r'\s*#include\s*"Dreadlock\.h"' # no capture
            changed = False
            new_lines = []
            reverts_found = 0

            with open(module) as f:
                for line in f:
                    result = re.search(dreadlock_include_re, line)
                    if result == None:
                        line = line.rstrip()
                        if 'DREADLOCK' in line:
                            dreadlock_ndx = line.find('DREADLOCK')

                            if '{{' in line:
                                changed = True
                                reverts_found += 1

                                original_start_ndx = line.find('{{')
                                original_end_ndx = line.find('}}')

                                line = line[original_start_ndx+2:original_end_ndx]
                                new_lines.append(line)
                            else:
                                pass  # we don't capture this line (it was automatically added)
                        else:
                            new_lines.append(line)

            if changed:
                if not options.dry_run:
                    if not options.overwrite:
                        print('\n'.join(new_lines))
                    else:
                        with open(module, 'w') as f:
                            f.write('\n'.join(new_lines))
            else:
                print("No revert markers were found in '%s'! (Did you explicitly disable revert for this file?)" % module)
