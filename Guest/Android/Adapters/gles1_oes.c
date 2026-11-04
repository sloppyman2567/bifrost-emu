// OpenGL ES 1.x entry points that an ES 2 context does not have, for apps that link libGLESv1_CM.
//
// SDL2's GLES1 renderer is such an app: its library imports the ES 1 names even when it draws through an ES 2 context, and
// Android's linker binds every import when it loads the library, so one missing name keeps the whole game from starting.
// They are exported here, not thunked: the host driver may not offer ES 1 names to an ES 2 context.
//
// GL_OES_framebuffer_object, GL_OES_blend_subtract, GL_OES_blend_equation_separate and GL_OES_blend_func_separate are
// the ES 2 core commands under another name. The arguments are identical, and so are the token values (all 42 enums of
// these extensions were compared with the core ones in the vendored Khronos registry, ThirdParty/GLRegistry/gl.xml;
// GL_FRAMEBUFFER_INCOMPLETE_FORMATS_OES has no core name and is only ever returned). Each wrapper forwards unchanged.
//
// GL_OES_draw_texture and the fixed-function commands of ES 1.x itself (glColor4f, glMatrixMode, glVertexPointer, ...;
// tools/Android/build-android-adapters.py generates their stubs from the registry) have no ES 2 equivalent: only an ES 1
// context can use them, and Bifrost does not provide one. They are exported so the library loads, and the first call
// says so on stderr (once per process, barring a race) instead of drawing nothing in silence.
#include "bionic.h"

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef unsigned char GLboolean;
typedef int GLint;
typedef int GLsizei;
typedef short GLshort;
typedef int GLfixed;
typedef float GLfloat;

extern GLboolean glIsRenderbuffer(GLuint);
extern void glBindRenderbuffer(GLenum, GLuint);
extern void glDeleteRenderbuffers(GLsizei, const GLuint *);
extern void glGenRenderbuffers(GLsizei, GLuint *);
extern void glRenderbufferStorage(GLenum, GLenum, GLsizei, GLsizei);
extern void glGetRenderbufferParameteriv(GLenum, GLenum, GLint *);
extern GLboolean glIsFramebuffer(GLuint);
extern void glBindFramebuffer(GLenum, GLuint);
extern void glDeleteFramebuffers(GLsizei, const GLuint *);
extern void glGenFramebuffers(GLsizei, GLuint *);
extern GLenum glCheckFramebufferStatus(GLenum);
extern void glFramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint);
extern void glFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint);
extern void glGetFramebufferAttachmentParameteriv(GLenum, GLenum, GLenum, GLint *);
extern void glGenerateMipmap(GLenum);
extern void glBlendEquation(GLenum);
extern void glBlendEquationSeparate(GLenum, GLenum);
extern void glBlendFuncSeparate(GLenum, GLenum, GLenum, GLenum);

#define EXPORT __attribute__((visibility("default")))

// ── GL_OES_framebuffer_object ─────────────────────────────────────────
EXPORT GLboolean glIsRenderbufferOES(GLuint renderbuffer) { return glIsRenderbuffer(renderbuffer); }
EXPORT void glBindRenderbufferOES(GLenum target, GLuint renderbuffer) { glBindRenderbuffer(target, renderbuffer); }
EXPORT void glDeleteRenderbuffersOES(GLsizei n, const GLuint *renderbuffers) { glDeleteRenderbuffers(n, renderbuffers); }
EXPORT void glGenRenderbuffersOES(GLsizei n, GLuint *renderbuffers) { glGenRenderbuffers(n, renderbuffers); }
EXPORT void glRenderbufferStorageOES(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
    glRenderbufferStorage(target, internalformat, width, height);
}
EXPORT void glGetRenderbufferParameterivOES(GLenum target, GLenum pname, GLint *params) {
    glGetRenderbufferParameteriv(target, pname, params);
}
EXPORT GLboolean glIsFramebufferOES(GLuint framebuffer) { return glIsFramebuffer(framebuffer); }
EXPORT void glBindFramebufferOES(GLenum target, GLuint framebuffer) { glBindFramebuffer(target, framebuffer); }
EXPORT void glDeleteFramebuffersOES(GLsizei n, const GLuint *framebuffers) { glDeleteFramebuffers(n, framebuffers); }
EXPORT void glGenFramebuffersOES(GLsizei n, GLuint *framebuffers) { glGenFramebuffers(n, framebuffers); }
EXPORT GLenum glCheckFramebufferStatusOES(GLenum target) { return glCheckFramebufferStatus(target); }
EXPORT void glFramebufferRenderbufferOES(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) {
    glFramebufferRenderbuffer(target, attachment, renderbuffertarget, renderbuffer);
}
EXPORT void glFramebufferTexture2DOES(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    glFramebufferTexture2D(target, attachment, textarget, texture, level);
}
EXPORT void glGetFramebufferAttachmentParameterivOES(GLenum target, GLenum attachment, GLenum pname, GLint *params) {
    glGetFramebufferAttachmentParameteriv(target, attachment, pname, params);
}
EXPORT void glGenerateMipmapOES(GLenum target) { glGenerateMipmap(target); }

// ── GL_OES_blend_subtract, GL_OES_blend_equation_separate, GL_OES_blend_func_separate ─
EXPORT void glBlendEquationOES(GLenum mode) { glBlendEquation(mode); }
EXPORT void glBlendEquationSeparateOES(GLenum modeRGB, GLenum modeAlpha) { glBlendEquationSeparate(modeRGB, modeAlpha); }
EXPORT void glBlendFuncSeparateOES(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) {
    glBlendFuncSeparate(srcRGB, dstRGB, srcAlpha, dstAlpha);
}

// ── Not available to an ES 2 context ──────────────────────────────────
// Also called by the generated stubs of the fixed-function commands.
__attribute__((visibility("hidden"))) void bifrost_gles1_unsupported(const char *name) {
    static volatile int reported;  // not atomic (no libgcc here): two racing threads may both say it
    if (reported) return;
    reported = 1;
    static const char suffix[] = " is OpenGL ES 1.x fixed-function API, which is not supported: Bifrost draws with "
                                 "OpenGL ES 2 and later. Nothing is drawn.\n";
    (void)!write(2, name, strlen(name));
    (void)!write(2, suffix, sizeof(suffix) - 1);
}

EXPORT void glDrawTexsOES(GLshort x, GLshort y, GLshort z, GLshort width, GLshort height) {
    (void)x; (void)y; (void)z; (void)width; (void)height; bifrost_gles1_unsupported("glDrawTexsOES");
}
EXPORT void glDrawTexiOES(GLint x, GLint y, GLint z, GLint width, GLint height) {
    (void)x; (void)y; (void)z; (void)width; (void)height; bifrost_gles1_unsupported("glDrawTexiOES");
}
EXPORT void glDrawTexxOES(GLfixed x, GLfixed y, GLfixed z, GLfixed width, GLfixed height) {
    (void)x; (void)y; (void)z; (void)width; (void)height; bifrost_gles1_unsupported("glDrawTexxOES");
}
EXPORT void glDrawTexfOES(GLfloat x, GLfloat y, GLfloat z, GLfloat width, GLfloat height) {
    (void)x; (void)y; (void)z; (void)width; (void)height; bifrost_gles1_unsupported("glDrawTexfOES");
}
EXPORT void glDrawTexsvOES(const GLshort *coords) { (void)coords; bifrost_gles1_unsupported("glDrawTexsvOES"); }
EXPORT void glDrawTexivOES(const GLint *coords) { (void)coords; bifrost_gles1_unsupported("glDrawTexivOES"); }
EXPORT void glDrawTexxvOES(const GLfixed *coords) { (void)coords; bifrost_gles1_unsupported("glDrawTexxvOES"); }
EXPORT void glDrawTexfvOES(const GLfloat *coords) { (void)coords; bifrost_gles1_unsupported("glDrawTexfvOES"); }
