// Host tests: the real core modules, attached through CoreRuntime like the
// firmware, driven by events; a recorder captures everything posted.
// Each file's tests are a boost.ut suite; this runs them. ut prints the
// failures and the summary, and the exit status is 0 only if every expect()
// held. Its command line (--help), e.g.:
//
//   sunsim_tests                             every test
//   sunsim_tests "<name>"                    the test so named (`*` and `?`
//                                            match anything)
//   sunsim_tests --list-test-names-only      each test's name, a line apiece
//                                            (how CTest finds them:
//                                            discover_tests.cmake)
//   sunsim_tests "<name>" --use-colour no    as CTest runs them: plain text,
//                                            where VS Code's Testing panel
//                                            finds the failing line

import std.compat;
import boost.ut;

int main(int argc, const char **argv)
{
    return boost::ut::cfg<>.run({.report_errors = true, .argc = argc, .argv = argv});
}
