// Minimal AGX workload generator: renders to an offscreen FBO for a while, so GPU submissions are
// in flight while DRM_IOCTL_ASAHI_GET_STATS is polled from another shell.
//
// Build and run (no EGL dev package needed beyond libglvnd):
//   nix-shell -p gcc15 libglvnd mesa --run \
//     "gcc -O2 -Wall -o /tmp/gpuload tools/asahi_gpuload.c -lEGL -lGLESv2"
//   EGL_PLATFORM=surfaceless GALLIUM_DRIVER=asahi EGL_LOG_LEVEL=debug /tmp/gpuload 30 400 0
//
// argv: [seconds] [inner loops per pixel] [gap in ms between submissions, 0 = back to back]
//
// No window system is involved: EGL uses the surfaceless platform, which talks to
// /dev/dri/renderD128 directly. GALLIUM_DRIVER=asahi makes Mesa fail loudly instead of quietly
// falling back to llvmpipe; EGL_LOG_LEVEL=debug prints "using driver asahi".

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char* frag_src =
	"precision highp float;\n"
	"uniform int loops;\n"
	"varying vec2 uv;\n"
	"void main() {\n"
	"  float a = uv.x * 3.0 + uv.y * 7.0;\n"
	"  for (int i = 0; i < loops; i++) { a = fract(sin(a * 12.9898) * 43758.5453); }\n"
	"  gl_FragColor = vec4(a, a, a, 1.0);\n"
	"}\n";

static const char* vert_src =
	"attribute vec2 pos;\n"
	"varying vec2 uv;\n"
	"void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n";

int main(int argc, char** argv) {
	const int seconds = argc > 1 ? atoi(argv[1]) : 20;
	const int loops = argc > 2 ? atoi(argv[2]) : 300;
	const int gap_ms = argc > 3 ? atoi(argv[3]) : 0;

	EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
		fprintf(stderr, "eglInitialize failed: 0x%x (try EGL_PLATFORM=surfaceless)\n", eglGetError());
		return 1;
	}

	const EGLint attribs[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_NONE
	};
	EGLConfig config;
	EGLint num_configs = 0;
	if (!eglChooseConfig(dpy, attribs, &config, 1, &num_configs) || num_configs == 0) {
		fprintf(stderr, "eglChooseConfig failed: 0x%x\n", eglGetError());
		return 1;
	}

	const EGLint pb_attribs[] = { EGL_WIDTH, 1024, EGL_HEIGHT, 1024, EGL_NONE };
	EGLSurface surf = eglCreatePbufferSurface(dpy, config, pb_attribs);
	if (surf == EGL_NO_SURFACE) {
		fprintf(stderr, "eglCreatePbufferSurface failed: 0x%x\n", eglGetError());
		return 1;
	}

	const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attribs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surf, surf, ctx)) {
		fprintf(stderr, "eglCreateContext/eglMakeCurrent failed: 0x%x\n", eglGetError());
		return 1;
	}

	printf("EGL: %s / %s\n", eglQueryString(dpy, EGL_VENDOR), eglQueryString(dpy, EGL_CLIENT_APIS));

	const GLuint quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	GLuint vbo, fbo, tex;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1024, 1024, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "framebuffer incomplete: 0x%x\n", glCheckFramebufferStatus(GL_FRAMEBUFFER));
		return 1;
	}

	GLuint vs = glCreateShader(GL_VERTEX_SHADER);
	GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(vs, 1, &vert_src, NULL);
	glShaderSource(fs, 1, &frag_src, NULL);
	glCompileShader(vs);
	glCompileShader(fs);
    GLint ok = GL_FALSE;
	glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024];
		glGetShaderInfoLog(fs, sizeof(log), NULL, log);
		fprintf(stderr, "fragment shader: %s\n", log);
		return 1;
	}

	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glBindAttribLocation(prog, 0, "pos");
	glLinkProgram(prog);
	glUseProgram(prog);
	glUniform1i(glGetUniformLocation(prog, "loops"), loops);

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glViewport(0, 0, 1024, 1024);

	const int max_loops = 20000;
	if (loops > max_loops) glUniform1i(glGetUniformLocation(prog, "loops"), max_loops);

	printf("rendering %d s of 1024x1024 shading (%d inner loops per pixel, %d ms gap)\n", seconds, loops, gap_ms);

	const struct timespec gap = { gap_ms / 1000, (long)(gap_ms % 1000) * 1000 * 1000 };
	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);
	int frame = 0;
	for (;;) {
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glFinish();                     /* hand the submission to the firmware and wait */
		if (gap_ms > 0) nanosleep(&gap, NULL);
		if ((frame % 100) == 0) printf("  frame %d\n", frame);
		frame++;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - start.tv_sec >= seconds) break;
	}

	printf("done\n");
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(dpy, ctx);
	eglDestroySurface(dpy, surf);
	eglTerminate(dpy);
	return 0;
}
