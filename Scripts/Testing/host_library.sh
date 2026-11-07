# Sourced by host harnesses that link libbifrost.a. build_host_library LOG builds the library of the
# configuration under test and sets HOST_LIB_DIR to its build directory. run_tests.sh names that
# emulator in BIFROST_EMU (build/release-sdl<0|1>-gl<0|1>/bifrost-emu); a harness run by hand, or with
# another emulator path, gets the headless library. Linking the configuration the job already built
# keeps the harness from compiling a whole library inside its timeout.
build_host_library() {
    local log=$1 sdl=0 gl=0
    if [[ ${BIFROST_EMU:-} =~ release-sdl([01])-gl([01])/bifrost-emu$ ]]; then
        sdl=${BASH_REMATCH[1]} gl=${BASH_REMATCH[2]}
    fi
    make USE_SDL2="$sdl" USE_THUNK_GL="$gl" lib > "$log" 2>&1 || { cat "$log" >&2; exit 1; }
    HOST_LIB_DIR=build/release-sdl$sdl-gl$gl
}
