# The source lints every build runs: the comment-length gate (AGENTS.md,
# "Comments -- write fewer, and shorter") and the names <windows.h> takes.
#
# Include LAST from a project: it makes every target defined so far in the
# including directory depend on the checks, so `--target spirula` fails as
# early as a full build does. Self-locating, so the standalone viewer build
# (viewer/CMakeLists.txt) can include it by relative path.
# ---------------------------------------------------------------------------

option(SS_CHECK_COMMENTS "Fail the build on an over-budget comment block in an uncommitted change" ON)

get_filename_component(ss_checks_root ${CMAKE_CURRENT_LIST_DIR} DIRECTORY)

# Neither is a build dependency of this project: a checkout with no python and
# no git still builds, it just checks nothing.
find_program(SS_PYTHON NAMES python3 python)
if(NOT SS_PYTHON)
    message(STATUS "python not found -- skipping the source lints")
    return()
endif()

# --quiet prints nothing while the tree is clean; a violation still reports in
# full. USES_TERMINAL keeps that report unbuffered under ninja.
set(ss_checks ss_check_winmacro)
add_custom_target(ss_check_winmacro ALL
    COMMAND ${SS_PYTHON} ${ss_checks_root}/tools/check_winmacro.py --quiet
    WORKING_DIRECTORY ${ss_checks_root}
    COMMENT "Checking sources for names <windows.h> defines as macros"
    VERBATIM USES_TERMINAL)

if(SS_CHECK_COMMENTS)
    list(APPEND ss_checks ss_check_comment_length)
    add_custom_target(ss_check_comment_length ALL
        COMMAND ${SS_PYTHON} ${ss_checks_root}/tools/check_comment_length.py --quiet
        WORKING_DIRECTORY ${ss_checks_root}
        COMMENT "Checking comment lengths in uncommitted changes"
        VERBATIM USES_TERMINAL)
endif()

get_property(ss_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(t IN LISTS ss_targets)
    if(NOT t IN_LIST ss_checks)
        add_dependencies(${t} ${ss_checks})
    endif()
endforeach()
