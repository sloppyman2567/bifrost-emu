// Guest-visible OpenGL ES extension list. Host extension strings pass
// through except the entries in Include/Generated/GLExtensions.hpp, whose
// commands have no thunk row (guests call advertised entry points without
// NULL checks). Desktop GL contexts keep the host list.
#pragma once
#include <cstdint>

namespace arm64emu::gl_extensions {

// glGetString(GL_EXTENSIONS): the filtered list, or nullptr when the
// current context is not OpenGL ES (use the host string).
const char* filtered_string();
// glGetStringi(GL_EXTENSIONS, index): true when the context is OpenGL ES;
// *name is nullptr for an index past the filtered list.
bool filtered_at(uint32_t index, const char** name);
// glGetIntegerv(GL_NUM_EXTENSIONS): true when the context is OpenGL ES.
bool filtered_count(int32_t* count);

}  // namespace arm64emu::gl_extensions
