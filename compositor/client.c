/*
 * Copyright © 2013 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-egl.h>
#include <wayland-cursor.h>

#include <GLES2/gl2.h>
#include <EGL/egl.h>

struct window;
struct seat;

struct nested_client {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
  struct wl_shell *shell;

	EGLDisplay egl_display;
	EGLContext egl_context;
	EGLConfig egl_config;
	EGLSurface egl_surface;
	struct program *color_program;

	GLuint vert, frag, program;
	GLuint rotation;
	GLuint pos;
	GLuint col;

	struct wl_surface *surface;
	struct wl_egl_window *native;
	int width, height;
};

#define POS 0
#define COL 1

static GLuint
create_shader(const char *source, GLenum shader_type)
{
	GLuint shader;
	GLint status;

	shader = glCreateShader(shader_type);
	if (shader == 0)
		return 0;

	glShaderSource(shader, 1, (const char **) &source, NULL);
	glCompileShader(shader);

	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status) {
		char log[1000];
		GLsizei len;
		glGetShaderInfoLog(shader, 1000, &len, log);
		fprintf(stderr, "Error: compiling %s: %*s\n",
			shader_type == GL_VERTEX_SHADER ? "vertex" : "fragment",
			len, log);
		return 0;
	}

	return shader;
}

static void
create_program(struct nested_client *client,
	       const char *vert, const char *frag)
{
	GLint status;

	client->vert = create_shader(vert, GL_VERTEX_SHADER);
	client->frag = create_shader(frag, GL_FRAGMENT_SHADER);

	client->program = glCreateProgram();
	glAttachShader(client->program, client->frag);
	glAttachShader(client->program, client->vert);
	glBindAttribLocation(client->program, POS, "pos");
	glBindAttribLocation(client->program, COL, "color");
	glLinkProgram(client->program);

	glGetProgramiv(client->program, GL_LINK_STATUS, &status);
	if (!status) {
		char log[1000];
		GLsizei len;
		glGetProgramInfoLog(client->program, 1000, &len, log);
		fprintf(stderr, "Error: linking:\n%*s\n", len, log);
		exit(1);
	}

	client->rotation =
		glGetUniformLocation(client->program, "rotation");
}

static const char vertex_shader_text[] =
	"uniform mat4 rotation;\n"
	"attribute vec4 pos;\n"
	"attribute vec4 color;\n"
	"varying vec4 v_color;\n"
	"void main() {\n"
	"  gl_Position = rotation * pos;\n"
	"  v_color = color;\n"
	"}\n";

static const char color_fragment_shader_text[] =
	"precision mediump float;\n"
	"varying vec4 v_color;\n"
	"void main() {\n"
	"  gl_FragColor = v_color;\n"
	"}\n";

static void
render_triangle(struct nested_client *client, uint32_t time)
{
	static const GLfloat verts[3][2] = {
		{ -0.5, -0.5 },
		{  0.5, -0.5 },
		{  0,    0.5 }
	};
	static const GLfloat colors[3][3] = {
		{ 1, 0, 0 },
		{ 0, 1, 0 },
		{ 0, 0, 1 }
	};
	static GLfloat angle;
	GLfloat rotation[4][4] = {
		{ 1, 0, 0, 0 },
		{ 0, 1, 0, 0 },
		{ 0, 0, 1, 0 },
		{ 0, 0, 0, 1 }
	};

	if (client->program == 0)
		create_program(client, vertex_shader_text,
			       color_fragment_shader_text);

	angle += 0.1f;
	rotation[0][0] =  cos(angle);
	rotation[0][2] =  sin(angle);
	rotation[2][0] = -sin(angle);
	rotation[2][2] =  cos(angle);
//  printf ("client: rotation: %.2f\n", angle);

	glClearColor(0.4, 0.4, 0.4, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(client->program);

	glViewport(0, 0, client->width, client->height);

	glUniformMatrix4fv(client->rotation, 1, GL_FALSE, (GLfloat *) rotation);

	glVertexAttribPointer(POS, 2, GL_FLOAT, GL_FALSE, 0, verts);
	glVertexAttribPointer(COL, 3, GL_FLOAT, GL_FALSE, 0, colors);
	glEnableVertexAttribArray(POS);
	glEnableVertexAttribArray(COL);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	glDisableVertexAttribArray(POS);
	glDisableVertexAttribArray(COL);

	glFlush();
}

static void
frame_callback(void *data, struct wl_callback *callback, uint32_t time);

static const struct wl_callback_listener frame_listener = {
	frame_callback
};

static void
frame_callback(void *data, struct wl_callback *callback, uint32_t time)
{
//  printf ("client: frame_callback start\n");
	struct nested_client *client = data;

	if (callback)
		wl_callback_destroy(callback);

	callback = wl_surface_frame(client->surface);
	wl_callback_add_listener(callback, &frame_listener, client);

	render_triangle(client, time);

	eglSwapBuffers(client->egl_display, client->egl_surface);
//  printf ("client: frame_callback end\n");
}

static void
registry_handle_global(void *data, struct wl_registry *registry,
		       uint32_t name, const char *interface, uint32_t version)
{
	struct nested_client *client = data;

	if (strcmp(interface, "wl_compositor") == 0) {
		client->compositor =
			wl_registry_bind(registry, name,
					 &wl_compositor_interface, 1);
	}
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
			      uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove
};

static struct nested_client *
nested_client_create(int width, int height)
{
	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	static const EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_ALPHA_SIZE, 1,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};

	EGLint major, minor, n;
	EGLBoolean ret;

	struct nested_client *client;

	client = malloc(sizeof *client);
	if (client == NULL)
		return NULL;

	client->width  = width;
	client->height = height;

	client->display = wl_display_connect(NULL);

	client->registry = wl_display_get_registry(client->display);
	wl_registry_add_listener(client->registry,
				 &registry_listener, client);

	/* get globals */
	wl_display_roundtrip(client->display);

	client->egl_display = eglGetDisplay(client->display);
	if (client->egl_display == NULL)
		return NULL;

	ret = eglInitialize(client->egl_display, &major, &minor);
	if (!ret)
		return NULL;
	ret = eglBindAPI(EGL_OPENGL_ES_API);
	if (!ret)
		return NULL;

	ret = eglChooseConfig(client->egl_display, config_attribs,
			      &client->egl_config, 1, &n);
	if (!ret || n != 1)
		return NULL;

	client->egl_context = eglCreateContext(client->egl_display,
					       client->egl_config,
					       EGL_NO_CONTEXT,
					       context_attribs);
	if (!client->egl_context)
		return NULL;

	client->surface = wl_compositor_create_surface(client->compositor);

	client->native = wl_egl_window_create(client->surface,
					      client->width, client->height);


	client->egl_surface =
		eglCreateWindowSurface(client->egl_display,
				       client->egl_config,
				       client->native, NULL);

	eglMakeCurrent(client->egl_display, client->egl_surface,
		       client->egl_surface, client->egl_context);

	wl_egl_window_resize(client->native,
			     client->width, client->height, 0, 0);

	frame_callback(client, NULL, 0);

	return client;
}

static void
nested_client_destroy(struct nested_client *client)
{
	eglMakeCurrent(client->egl_display,
		       EGL_NO_SURFACE, EGL_NO_SURFACE,
		       EGL_NO_CONTEXT);

	wl_egl_window_destroy(client->native);

	wl_surface_destroy(client->surface);

	if (client->compositor)
		wl_compositor_destroy(client->compositor);

	wl_registry_destroy(client->registry);
	wl_display_flush(client->display);
	wl_display_disconnect(client->display);
}

int
main(int argc, char **argv)
{
	struct nested_client *client;
	int ret = 0;

	if (getenv("WAYLAND_SOCKET") == NULL) {
		fprintf(stderr, "must be run by nested, don't run standalone\n");
		return -1;
	}

//  printf ("client: program started\n");

	client = nested_client_create(400, 300);

	while (ret != -1) {
		ret = wl_display_dispatch(client->display);
  }

	nested_client_destroy(client);

//  printf ("client: program finished\n");

	return 0;
}
