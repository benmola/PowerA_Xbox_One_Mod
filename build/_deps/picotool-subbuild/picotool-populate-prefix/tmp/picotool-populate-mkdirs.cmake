# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "G:/PowerA Controller Project/PowerA_Xbox_One_Modd_New/PowerA_Xbox_One_Mod/build/_deps/picotool-src")
  file(MAKE_DIRECTORY "G:/PowerA Controller Project/PowerA_Xbox_One_Modd_New/PowerA_Xbox_One_Mod/build/_deps/picotool-src")
endif()
file(MAKE_DIRECTORY
  "G:/PowerA Controller Project/PowerA_Xbox_One_Modd_New/PowerA_Xbox_One_Mod/build/_deps/picotool-build"
  "G:/PowerA Controller Project/PowerA_Xbox_One_Modd_New/PowerA_Xbox_One_Mod/build/_deps/picotool-subbuild/picotool-populate-prefix"
  "G:/PowerA Controller Project/PowerA_Xbox_One_Modd_New/PowerA_Xbox_One_Mod/build/_deps/picotool-subbuild/picotool-populate-prefix/tmp"
  "G:/PowerA Controller Project/PowerA_Xbox_One_Modd_New/PowerA_Xbox_One_Mod/build/_deps/picotool-subbuild/picotool-populate-prefix/src/picotool-populate-stamp"
  "G:/PowerA Controller Project/PowerA_Xbox_One_Modd_New/PowerA_Xbox_One_Mod/build/_deps/picotool-subbuild/picotool-populate-prefix/src"
  "G:/PowerA Controller Project/PowerA_Xbox_One_Modd_New/PowerA_Xbox_One_Mod/build/_deps/picotool-subbuild/picotool-populate-prefix/src/picotool-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "G:/PowerA Controller Project/PowerA_Xbox_One_Modd_New/PowerA_Xbox_One_Mod/build/_deps/picotool-subbuild/picotool-populate-prefix/src/picotool-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "G:/PowerA Controller Project/PowerA_Xbox_One_Modd_New/PowerA_Xbox_One_Mod/build/_deps/picotool-subbuild/picotool-populate-prefix/src/picotool-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
