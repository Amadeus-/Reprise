# Reprise fork-local build flags. Kept OUT of the upstream CMake files so it never
# conflicts on an `+Update (from upstream)+` merge; wired in by a single include()
# line in the top-level CMakeLists.txt (search that file for "RepriseBuildFlags").
#
# Purpose: silence the C4702 "unreachable code" flood that Qt 6.11 headers emit under
# /W4 (e.g. Qt/.../QtCore/qvariant.h(723)). We scope the suppression to EXTERNAL
# (angle-bracket) headers only, so our OWN code still gets C4702 and the fork keeps its
# zero-warnings discipline -- preferred over a blanket /wd4702. Qt and other third-party
# headers are included with <>, our own headers with "", so only vendor headers go quiet.
#
# /external:anglebrackets - treat every <>-included header as an external header
# /external:W0            - compile external headers at warning level 0
# /external is a stable MSVC feature since VS 2019 16.10 (no /experimental needed).

if (MSVC)
    add_compile_options(/external:anglebrackets /external:W0)

    # Emit a SEPARATE .pdb for Release too (symbols live in the .pdb, NOT embedded in the
    # exe/dll) so any crash can be symbolized. Debug / RelWithDebInfo already produce PDBs;
    # stock Release does not. Use /Z7 (debug info carried IN the .obj files -- which are
    # intermediate and never shipped) rather than /Zi: /Zi needs a shared compiler .pdb that
    # collides with the precompiled headers under the /MP parallel build (error C2859
    # "pdb ... is not the pdb file that was used when this precompiled header was created").
    # /DEBUG makes the linker harvest the /Z7 debug info into the final target .pdb; the
    # shipped exe/dll stays lean. /OPT:REF + /OPT:ICF restore the Release size optimizations
    # that a bare /DEBUG would otherwise disable.
    add_compile_options($<$<CONFIG:Release>:/Z7>)
    add_link_options($<$<CONFIG:Release>:/DEBUG>)
    add_link_options($<$<CONFIG:Release>:/OPT:REF>)
    add_link_options($<$<CONFIG:Release>:/OPT:ICF>)
endif()

# --- Reprise: README version-drift guard (enforces the version-bump checklist at build time) ---
# A version bump in version.cmake MUST also refresh README.md's "Pre-release (X.Y.Z)" line (plus its
# CHANGELOG entry + any affected feature bullets). This guard turns a README/version.cmake mismatch
# into a hard configure FATAL_ERROR, so a bump can never go green with a stale README. It runs on every
# real configure (this file is include()d from the top-level CMakeLists AFTER SetupConfigure has set
# MUSE_APP_VERSION) and on all platforms. Deliberately NOT placed in version.cmake itself, so the CI
# `cmake -P version.cmake` version-parse path (make_version_env.sh etc.) stays unaffected. Fork-owned +
# merge-safe (this file never conflicts on an upstream merge). CMAKE_CURRENT_LIST_DIR here = repo root.
if (DEFINED MUSE_APP_VERSION AND MUSE_APP_VERSION)
    set(_reprise_readme "${CMAKE_CURRENT_LIST_DIR}/README.md")
    if (EXISTS "${_reprise_readme}")
        file(READ "${_reprise_readme}" _reprise_readme_text)
        string(REGEX MATCH "Pre-release \\(([0-9]+\\.[0-9]+\\.[0-9]+)\\)" _reprise_readme_ver "${_reprise_readme_text}")
        if (NOT _reprise_readme_ver)
            message(FATAL_ERROR "Reprise: README.md has no 'Pre-release (X.Y.Z)' line to match against version.cmake (${MUSE_APP_VERSION}). Add/fix that line in README.md before building.")
        elseif (NOT CMAKE_MATCH_1 STREQUAL MUSE_APP_VERSION)
            message(FATAL_ERROR "Reprise: README.md version (${CMAKE_MATCH_1}) does not match version.cmake (${MUSE_APP_VERSION}). Refresh README.md's 'Pre-release (...)' line + CHANGELOG + any affected bullets to ${MUSE_APP_VERSION} before building.")
        endif()
        message(STATUS "Reprise README version guard: README (${CMAKE_MATCH_1}) == version.cmake (${MUSE_APP_VERSION}) -- OK")
    endif()
endif()
