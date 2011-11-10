"""Run all unit tests."""

# Copyright (C) 2006-2007 Anders Logg
#
# This file is part of DOLFIN.
#
# DOLFIN is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# DOLFIN is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with DOLFIN. If not, see <http://www.gnu.org/licenses/>.
#
# Modified by Johannes Ring 2009, 2011
# Modified by Garth N. Wells 2009-2011
#
# First added:  2006-08-09
# Last changed: 2011-08-31

import sys, os, re
import platform
import instant
from dolfin_utils.commands import getstatusoutput
from dolfin import has_mpi, has_parmetis

# Tests to run
tests = {
    "adaptivity":     ["errorcontrol", "TimeSeries"],
    "book":           ["chapter_1", "chapter_10"],
    "fem":            ["solving", "Assembler", "DirichletBC", "DofMap",
                       "FiniteElement", "SystemAssembler"],
    "function":       ["test", "Constant", "Expression", "Function",
                       "FunctionSpace"],
    "io":             ["test", "vtk", "XMLMeshFunction", "XMLMesh",
                       "XMLMeshValueCollection", "XMLVector"],
    "jit":            ["test"],
    "la":             ["test", "Matrix", "Scalar", "Vector"],
    "math":           ["test"],
    "meshconvert":    ["test"],
    "mesh":           ["test", "Edge", "Face", "MeshData", "MeshEditor",
                       "MeshFunction", "MeshIterator", "MeshMarkers",
                       "MeshValueCollection"],
    "parameter":      ["test"],
    "python-extras":  ["test"],
    "quadrature":     ["test"],
    "refinement":     ["test"],
    }

# FIXME: Graph tests disabled for now since SCOTCH is now required

# Run both C++ and Python tests as default
only_python = False

# Check if we should run only Python tests, use for quick testing
if len(sys.argv) == 2 and sys.argv[1] == "--only-python":
    only_python = True

# Build prefix list
prefixes = [""]
if has_mpi() and has_parmetis():
    prefixes.append("mpirun -np 3 ")
else:
    print "DOLFIN has not been compiled with MPI and/or ParMETIS. Unit tests will not be run in parallel."

# Allow to disable parallel testing
if "DISABLE_PARALLEL_TESTING" in os.environ:
    prefixes = [""]

failed = []
# Run tests in serial, then in parallel
for prefix in prefixes:
    for test, subtests in tests.items():
        for subtest in subtests:
            print "Running unit tests for %s (%s) with prefix '%s'" % (test,  subtest, prefix)
            print "----------------------------------------------------------------------"

            cpptest_executable = subtest + "_" + test
            if platform.system() == 'Windows':
                cpptest_executable += '.exe'
            print "C++:   ",
            if not only_python and os.path.isfile(os.path.join(test, "cpp", cpptest_executable)):
                status, output = getstatusoutput("cd %s%scpp && %s .%s%s" % \
                                   (test, os.path.sep, prefix, os.path.sep, cpptest_executable))
                if status == 0 and "OK" in output:
                    num_tests = int(re.search("OK \((\d+)\)", output).groups()[0])
                    print "OK (%d tests)" % num_tests
                else:
                    print "*** Failed"
                    failed += [(test, subtest, "C++", output)]
            else:
                print "Skipping"

            print "Python:",
            if os.path.isfile(os.path.join(test, "python", subtest + ".py")):
                status, output = getstatusoutput("cd %s%spython && %s python .%s%s.py" % \
                                   (test, os.path.sep, prefix, os.path.sep, subtest))
                if status == 0 and "OK" in output:
                    num_tests = int(re.search("Ran (\d+) test", output).groups()[0])
                    print "OK (%d tests)" % num_tests
                else:
                    print "*** Failed"
                    failed += [(test, subtest, "Python", output)]
            else:
                print "Skipping"

            print ""

    # Print output for failed tests
    for (test, subtest, interface, output) in failed:
        print "One or more unit tests failed for %s (%s, %s):" % (test, subtest, interface)
        print output

# Return error code if tests failed
sys.exit(len(failed) != 0)
