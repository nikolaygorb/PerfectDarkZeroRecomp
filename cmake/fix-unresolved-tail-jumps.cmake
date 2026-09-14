# Cross-platform (cmake -P) replacement for the old fix-unresolved-tail-jumps.ps1.
# Re-applies the "cross-function tail-jump" workaround after every `rexglue
# codegen` run, since codegen regenerates these files from scratch and
# doesn't preserve hand edits. Safe to re-run: a file with no remaining
# broken gotos is left untouched.
#
# What it looks for: a `goto loc_XXXXXXXX;` whose target label isn't declared
# anywhere in the same generated .cpp file (PPC compilers sometimes emit a
# shared tail block split across two functions by rexglue's Discover/Merge
# phases). Each such goto is replaced with a logged early return instead,
# matching the pattern already established for sub_82401A40's two branches.
#
# Usage: cmake -DGENERATED_DIR=<path> -P fix-unresolved-tail-jumps.cmake

if(NOT DEFINED GENERATED_DIR)
    message(FATAL_ERROR "GENERATED_DIR must be passed via -DGENERATED_DIR=<path>")
endif()

file(GLOB cpp_files "${GENERATED_DIR}/*.cpp")

set(total_patched 0)

foreach(cpp_file ${cpp_files})
    file(STRINGS "${cpp_file}" lines)
    get_filename_component(fname "${cpp_file}" NAME)

    # Pass 1: record every declared label as a variable for O(1) lookup
    # (a plain CMake list would make this an O(n) scan per lookup - too slow
    # over the ~35k lines/~1k labels typical of one of these generated files).
    set(declared_labels)
    foreach(line ${lines})
        if(line MATCHES "^(loc_[0-9A-Fa-f]+):")
            set(_have_${CMAKE_MATCH_1} TRUE)
            list(APPEND declared_labels "${CMAKE_MATCH_1}")
        endif()
    endforeach()

    # Pass 2: cheap scan for gotos whose target wasn't declared in this file.
    # Deliberately doesn't rebuild the file's content here - rebuilding via
    # repeated list(APPEND) over tens of thousands of lines is O(n^2) and
    # takes seconds per file, even though this is a no-op on almost every run.
    set(broken_labels)
    foreach(line ${lines})
        if(line MATCHES "goto (loc_[0-9A-Fa-f]+);")
            set(label "${CMAKE_MATCH_1}")
            if(NOT _have_${label})
                list(APPEND broken_labels "${label}")
            endif()
        endif()
    endforeach()

    foreach(declared_label ${declared_labels})
        unset(_have_${declared_label})
    endforeach()

    # Only pay for a full read/patch/write when this file actually needs one.
    if(broken_labels)
        list(REMOVE_DUPLICATES broken_labels)
        file(READ "${cpp_file}" content)
        foreach(label ${broken_labels})
            string(REPLACE "goto ${label};"
                "{ REXLOG_WARN(\"unresolved tail-jump to ${label} (shared tail block in a different generated function)\"); return; }"
                content "${content}")
            math(EXPR total_patched "${total_patched}+1")
            message(STATUS "[fix-tail-jumps] ${fname}: patched unresolved goto ${label}")
        endforeach()
        file(WRITE "${cpp_file}" "${content}")
    endif()
endforeach()

message(STATUS "[fix-tail-jumps] done, ${total_patched} unresolved tail-jump(s) patched")
