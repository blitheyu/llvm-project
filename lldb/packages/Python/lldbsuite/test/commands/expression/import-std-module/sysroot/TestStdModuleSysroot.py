"""
Test that we respect the sysroot when building the std module.
"""

from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil
import os

class ImportStdModule(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    @skipIf(compiler=no_match("clang"))
    def test(self):
        self.build()

        sysroot = os.path.join(os.getcwd(), "root")

        # Set the sysroot.
        self.runCmd("platform select --sysroot '" + sysroot + "' host", CURRENT_EXECUTABLE_SET)

        lldbutil.run_to_source_breakpoint(self,
            "// Set break point at this line.", lldb.SBFileSpec("main.cpp"))

        self.runCmd("settings set target.import-std-module true")

        # Call our custom function in our sysroot std module.
        # If this gives us the correct result, then we used the sysroot.
        # We rely on the default argument of -123 to make sure we actually have the C++ module.
        # (We don't have default arguments in the debug information).
        self.expect("expr std::myabs()", substrs=['(int) $0 = 123'])
