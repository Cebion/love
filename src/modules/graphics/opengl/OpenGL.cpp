/**
 * Copyright (c) 2006-2015 LOVE Development Team
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 **/

// LOVE
#include "common/config.h"
#include "OpenGL.h"

#include "Shader.h"
#include "Canvas.h"
#include "common/Exception.h"

// C++
#include <algorithm>
#include <limits>

// C
#include <cstring>

namespace love
{
namespace graphics
{
namespace opengl
{

OpenGL::OpenGL()
	: stats()
	, contextInitialized(false)
	, maxAnisotropy(1.0f)
	, maxTextureSize(0)
	, maxRenderTargets(0)
	, vendor(VENDOR_UNKNOWN)
	, state()
{
	matrices.transform.reserve(10);
	matrices.projection.reserve(2);
}

bool OpenGL::initContext()
{
	if (contextInitialized)
		return true;

	if (!gladLoadGL())
		return false;

	initOpenGLFunctions();
	initVendor();
	initMatrices();

	// Store the current color so we don't have to get it through GL later.
	GLfloat glcolor[4];
	if (GLAD_ES_VERSION_2_0)
		glGetVertexAttribfv(GLuint(ATTRIB_COLOR), GL_CURRENT_VERTEX_ATTRIB, glcolor);
	else
		glGetFloatv(GL_CURRENT_COLOR, glcolor);
	state.color.r = glcolor[0] * 255;
	state.color.g = glcolor[1] * 255;
	state.color.b = glcolor[2] * 255;
	state.color.a = glcolor[3] * 255;

	// Same with the current clear color.
	glGetFloatv(GL_COLOR_CLEAR_VALUE, glcolor);
	state.clearColor.r = glcolor[0] * 255;
	state.clearColor.g = glcolor[1] * 255;
	state.clearColor.b = glcolor[2] * 255;
	state.clearColor.a = glcolor[3] * 255;

	// Get the current viewport.
	glGetIntegerv(GL_VIEWPORT, (GLint *) &state.viewport.x);

	// And the current scissor - but we need to compensate for GL scissors
	// starting at the bottom left instead of top left.
	glGetIntegerv(GL_SCISSOR_BOX, (GLint *) &state.scissor.x);
	state.scissor.y = state.viewport.h - (state.scissor.y + state.scissor.h);

	if (GLAD_VERSION_1_0)
		glGetFloatv(GL_POINT_SIZE, &state.pointSize);
	else
		state.pointSize = 1.0f;

	// Initialize multiple texture unit support for shaders, if available.
	state.textureUnits.clear();
	if (Shader::isSupported())
	{
		GLint maxtextureunits;
		glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxtextureunits);

		state.textureUnits.resize(maxtextureunits, 0);

		GLenum curgltextureunit;
		glGetIntegerv(GL_ACTIVE_TEXTURE, (GLint *) &curgltextureunit);

		state.curTextureUnit = (int) curgltextureunit - GL_TEXTURE0;

		// Retrieve currently bound textures for each texture unit.
		for (size_t i = 0; i < state.textureUnits.size(); i++)
		{
			glActiveTexture(GL_TEXTURE0 + i);
			glGetIntegerv(GL_TEXTURE_BINDING_2D, (GLint *) &state.textureUnits[i]);
		}

		glActiveTexture(curgltextureunit);
	}
	else
	{
		// Multitexturing not supported, so we only have 1 texture unit.
		state.textureUnits.resize(1, 0);
		state.curTextureUnit = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, (GLint *) &state.textureUnits[0]);
	}

	// This will be non-zero on some platforms.
	if (Canvas::isSupported())
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, (GLint *) &state.defaultFBO);

	BlendState blend = {GL_ONE, GL_ONE, GL_ZERO, GL_ZERO, GL_FUNC_ADD};
	setBlendState(blend);

	initMaxValues();
	createDefaultTexture();

	// Invalidate the cached matrices by setting some elements to NaN.
	float nan = std::numeric_limits<float>::quiet_NaN();
	state.lastProjectionMatrix.setTranslation(nan, nan);
	state.lastTransformMatrix.setTranslation(nan, nan);

	if (GLAD_VERSION_1_1)
		glMatrixMode(GL_MODELVIEW);

	contextInitialized = true;
	return true;
}

void OpenGL::deInitContext()
{
	if (!contextInitialized)
		return;

	glDeleteTextures(1, &state.defaultTexture);
	state.defaultTexture = 0;

	contextInitialized = false;
}

void OpenGL::initVendor()
{
	const char *vstr = (const char *) glGetString(GL_VENDOR);
	if (!vstr)
	{
		vendor = VENDOR_UNKNOWN;
		return;
	}

	// http://feedback.wildfiregames.com/report/opengl/feature/GL_VENDOR
	// http://stackoverflow.com/questions/2093594/opengl-extensions-available-on-different-android-devices
	if (strstr(vstr, "ATI Technologies"))
		vendor = VENDOR_ATI_AMD;
	else if (strstr(vstr, "NVIDIA"))
		vendor = VENDOR_NVIDIA;
	else if (strstr(vstr, "Intel"))
		vendor = VENDOR_INTEL;
	else if (strstr(vstr, "Mesa"))
		vendor = VENDOR_MESA_SOFT;
	else if (strstr(vstr, "Apple Computer"))
		vendor = VENDOR_APPLE;
	else if (strstr(vstr, "Microsoft"))
		vendor = VENDOR_MICROSOFT;
	else if (strstr(vstr, "Imagination"))
		vendor = VENDOR_IMGTEC;
	else if (strstr(vstr, "ARM"))
		vendor = VENDOR_ARM;
	else if (strstr(vstr, "Qualcomm"))
		vendor = VENDOR_QUALCOMM;
	else if (strstr(vstr, "Broadcom"))
		vendor = VENDOR_BROADCOM;
	else if (strstr(vstr, "Vivante"))
		vendor = VENDOR_VIVANTE;
	else
		vendor = VENDOR_UNKNOWN;
}

void OpenGL::initOpenGLFunctions()
{
	// The functionality of the core and ARB VBOs are identical, so we can
	// assign the pointers of the ARB functions to the names of the core
	// functions, if the latter isn't supported but the former is.
	if (GLAD_ARB_vertex_buffer_object && !GLAD_VERSION_1_5)
	{
		fp_glBindBuffer = (pfn_glBindBuffer) fp_glBindBufferARB;
		fp_glBufferData = (pfn_glBufferData) fp_glBufferDataARB;
		fp_glBufferSubData = (pfn_glBufferSubData) fp_glBufferSubDataARB;
		fp_glDeleteBuffers = (pfn_glDeleteBuffers) fp_glDeleteBuffersARB;
		fp_glGenBuffers = (pfn_glGenBuffers) fp_glGenBuffersARB;
		fp_glGetBufferParameteriv = (pfn_glGetBufferParameteriv) fp_glGetBufferParameterivARB;
		fp_glGetBufferPointerv = (pfn_glGetBufferPointerv) fp_glGetBufferPointervARB;
		fp_glGetBufferSubData = (pfn_glGetBufferSubData) fp_glGetBufferSubDataARB;
		fp_glIsBuffer = (pfn_glIsBuffer) fp_glIsBufferARB;
		fp_glMapBuffer = (pfn_glMapBuffer) fp_glMapBufferARB;
		fp_glUnmapBuffer = (pfn_glUnmapBuffer) fp_glUnmapBufferARB;
	}
}

void OpenGL::initMaxValues()
{
	// We'll need this value to clamp anisotropy.
	if (GLAD_EXT_texture_filter_anisotropic)
		glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAnisotropy);
	else
		maxAnisotropy = 1.0f;

	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);

	if (Canvas::isSupported() && (GLAD_VERSION_2_0 || GLAD_ARB_draw_buffers))
	{
		int maxattachments = 0;
		glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &maxattachments);

		int maxdrawbuffers = 0;
		glGetIntegerv(GL_MAX_DRAW_BUFFERS, &maxdrawbuffers);

		maxRenderTargets = std::min(maxattachments, maxdrawbuffers);
	}
	else
		maxRenderTargets = 0;
}

void OpenGL::initMatrices()
{
	matrices.transform.clear();
	matrices.projection.clear();

	matrices.transform.push_back(Matrix());
	matrices.projection.push_back(Matrix());
}

void OpenGL::createDefaultTexture()
{
	// Set the 'default' texture as a repeating white pixel. Otherwise,
	// texture2D calls inside a shader would return black when drawing graphics
	// primitives, which would create the need to use different "passthrough"
	// shaders for untextured primitives vs images.

	GLuint curtexture = state.textureUnits[state.curTextureUnit];

	glGenTextures(1, &state.defaultTexture);
	bindTexture(state.defaultTexture);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

	GLubyte pix = 255;
	glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 1, 1, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, &pix);

	bindTexture(curtexture);
}

void OpenGL::pushTransform()
{
	matrices.transform.push_back(matrices.transform.back());
}

void OpenGL::popTransform()
{
	matrices.transform.pop_back();
}

Matrix &OpenGL::getTransform()
{
	return matrices.transform.back();
}

void OpenGL::prepareDraw()
{
	const Matrix &transform = matrices.transform.back();
	const Matrix &proj = matrices.projection.back();

	Shader *shader = Shader::current;

	// Make sure the active shader has the correct values for its love-provided
	// uniforms.
	if (shader)
	{
		shader->checkSetScreenParams();

		// We need to make sure antialiased Canvases are properly resolved
		// before sampling from their textures in a shader.
		// This is kind of a big hack. :(
		for (auto &r : shader->getBoundRetainables())
		{
			// Even bigger hack! D:
			Canvas *canvas = dynamic_cast<Canvas *>(r.second);
			if (canvas != nullptr)
				canvas->resolveMSAA();
		}
	}

	if (GLAD_ES_VERSION_2_0 && shader)
	{
		// Send built-in uniforms to the current shader.
		shader->sendBuiltinMatrix(Shader::BUILTIN_TRANSFORM_MATRIX, 4, transform.getElements(), 1);
		shader->sendBuiltinMatrix(Shader::BUILTIN_PROJECTION_MATRIX, 4, proj.getElements(), 1);

		Matrix tp_matrix(proj * transform);
		shader->sendBuiltinMatrix(Shader::BUILTIN_TRANSFORM_PROJECTION_MATRIX, 4, tp_matrix.getElements(), 1);

		shader->sendBuiltinFloat(Shader::BUILTIN_POINT_SIZE, 1, &state.pointSize, 1);
	}
	else if (GLAD_VERSION_1_0)
	{
		const float *lastproj = state.lastProjectionMatrix.getElements();

		// We only need to re-upload the projection matrix if it's changed.
		if (memcmp(proj.getElements(), lastproj, sizeof(float) * 16) != 0)
		{
			glMatrixMode(GL_PROJECTION);
			glLoadMatrixf(proj.getElements());
			glMatrixMode(GL_MODELVIEW);

			state.lastProjectionMatrix = proj;
		}

		const float *lastxform = state.lastTransformMatrix.getElements();

		// Same with the transform matrix.
		if (memcmp(transform.getElements(), lastxform, sizeof(float) * 16) != 0)
		{
			glLoadMatrixf(transform.getElements());
			state.lastTransformMatrix = transform;
		}
	}
}

void OpenGL::drawArrays(GLenum mode, GLint first, GLsizei count)
{
	glDrawArrays(mode, first, count);
	++stats.drawCalls;
}

void OpenGL::drawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	glDrawElements(mode, count, type, indices);
	++stats.drawCalls;
}

void OpenGL::drawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint basevertex)
{
	glDrawElementsBaseVertex(mode, count, type, indices, basevertex);
	++stats.drawCalls;
}

void OpenGL::setColor(const Color &c)
{
	if (GLAD_ES_VERSION_2_0)
		glVertexAttrib4f(GLuint(ATTRIB_COLOR), c.r/255.f, c.g/255.f, c.b/255.f, c.a/255.f);
	else
		glColor4ubv(&c.r);

	state.color = c;
}

Color OpenGL::getColor() const
{
	return state.color;
}

void OpenGL::setClearColor(const Color &c)
{
	glClearColor(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f);
	state.clearColor = c;
}

Color OpenGL::getClearColor() const
{
	return state.clearColor;
}

GLint OpenGL::getGLAttrib(OpenGL::VertexAttrib attrib)
{
	if (GLAD_ES_VERSION_2_0)
	{
		// The enum value maps to a generic vertex attribute index.
		return GLint(attrib);
	}
	else
	{
		switch (attrib)
		{
		case ATTRIB_POS:
			return GL_VERTEX_ARRAY;
		case ATTRIB_TEXCOORD:
			return GL_TEXTURE_COORD_ARRAY;
		case ATTRIB_COLOR:
			return GL_COLOR_ARRAY;
		default:
			return GLint(attrib);
		}
	}

	return -1;
}

void OpenGL::enableVertexAttribArray(OpenGL::VertexAttrib attrib)
{
	GLint glattrib = getGLAttrib(attrib);

	if (GLAD_ES_VERSION_2_0)
		glEnableVertexAttribArray((GLuint) glattrib);
	else
		glEnableClientState((GLenum) glattrib);
}

void OpenGL::disableVertexAttribArray(OpenGL::VertexAttrib attrib)
{
	GLint glattrib = getGLAttrib(attrib);

	if (GLAD_ES_VERSION_2_0)
		glDisableVertexAttribArray((GLuint) glattrib);
	else
		glDisableClientState((GLenum) glattrib);
}

void OpenGL::setVertexAttribArray(OpenGL::VertexAttrib attrib, GLint size, GLenum type, GLsizei stride, const GLvoid *pointer)
{
	if (GLAD_ES_VERSION_2_0)
	{
		GLboolean normalized = (type == GL_UNSIGNED_BYTE) ? GL_TRUE : GL_FALSE;
		glVertexAttribPointer(GLuint(attrib), size, type, normalized, stride, pointer);
	}
	else
	{
		switch (attrib)
		{
		case ATTRIB_POS:
			glVertexPointer(size, type, stride, pointer);
			break;
		case ATTRIB_TEXCOORD:
			glTexCoordPointer(size, type, stride, pointer);
			break;
		case ATTRIB_COLOR:
			glColorPointer(size, type, stride, pointer);
			break;
		default:
			break;
		}
	}
}

void OpenGL::setViewport(const OpenGL::Viewport &v)
{
	glViewport(v.x, v.y, v.w, v.h);
	state.viewport = v;

	// glScissor starts from the lower left, so we compensate when setting the
	// scissor. When the viewport is changed, we need to manually update the
	// scissor again.
	setScissor(state.scissor);
}

OpenGL::Viewport OpenGL::getViewport() const
{
	return state.viewport;
}

void OpenGL::setScissor(const OpenGL::Viewport &v)
{
	if (Canvas::current)
		glScissor(v.x, v.y, v.w, v.h);
	else
	{
		// With no Canvas active, we need to compensate for glScissor starting
		// from the lower left of the viewport instead of the top left.
		glScissor(v.x, state.viewport.h - (v.y + v.h), v.w, v.h);
	}

	state.scissor = v;
}

OpenGL::Viewport OpenGL::getScissor() const
{
	return state.scissor;
}

void OpenGL::setBlendState(const BlendState &blend)
{
	if (GLAD_ES_VERSION_2_0 || GLAD_VERSION_1_4)
		glBlendEquation(blend.func);
	else if (GLAD_EXT_blend_minmax && GLAD_EXT_blend_subtract)
		glBlendEquationEXT(blend.func);
	else
	{
		if (blend.func == GL_FUNC_REVERSE_SUBTRACT)
			throw love::Exception("This graphics card does not support the subtractive blend mode!");
		// GL_FUNC_ADD is the default even without access to glBlendEquation, so that'll still work.
	}

	if (blend.srcRGB == blend.srcA && blend.dstRGB == blend.dstA)
		glBlendFunc(blend.srcRGB, blend.dstRGB);
	else
	{
		if (GLAD_ES_VERSION_2_0 || GLAD_VERSION_1_4)
			glBlendFuncSeparate(blend.srcRGB, blend.dstRGB, blend.srcA, blend.dstA);
		else if (GLAD_EXT_blend_func_separate)
			glBlendFuncSeparateEXT(blend.srcRGB, blend.dstRGB, blend.srcA, blend.dstA);
		else
			throw love::Exception("This graphics card does not support separated rgb and alpha blend functions!");
	}

	state.blend = blend;
}

OpenGL::BlendState OpenGL::getBlendState() const
{
	return state.blend;
}


void OpenGL::setPointSize(float size)
{
	if (GLAD_VERSION_1_0)
		glPointSize(size);

	state.pointSize = size;
}

float OpenGL::getPointSize() const
{
	return state.pointSize;
}

GLuint OpenGL::getDefaultFBO() const
{
	return state.defaultFBO;
}

GLuint OpenGL::getDefaultTexture() const
{
	return state.defaultTexture;
}

void OpenGL::setTextureUnit(int textureunit)
{
	if (textureunit < 0 || (size_t) textureunit >= state.textureUnits.size())
		throw love::Exception("Invalid texture unit index (%d).", textureunit);

	if (textureunit != state.curTextureUnit)
	{
		if (state.textureUnits.size() > 1)
			glActiveTexture(GL_TEXTURE0 + textureunit);
		else
			throw love::Exception("Multitexturing is not supported.");
	}

	state.curTextureUnit = textureunit;
}

void OpenGL::bindTexture(GLuint texture)
{
	if (texture != state.textureUnits[state.curTextureUnit])
	{
		state.textureUnits[state.curTextureUnit] = texture;
		glBindTexture(GL_TEXTURE_2D, texture);
	}
}

void OpenGL::bindTextureToUnit(GLuint texture, int textureunit, bool restoreprev)
{
	if (textureunit < 0 || (size_t) textureunit >= state.textureUnits.size())
		throw love::Exception("Invalid texture unit index.");

	if (texture != state.textureUnits[textureunit])
	{
		int oldtextureunit = state.curTextureUnit;
		setTextureUnit(textureunit);

		state.textureUnits[textureunit] = texture;
		glBindTexture(GL_TEXTURE_2D, texture);

		if (restoreprev)
			setTextureUnit(oldtextureunit);
	}
}

void OpenGL::deleteTexture(GLuint texture)
{
	// glDeleteTextures binds texture 0 to all texture units the deleted texture
	// was bound to before deletion.
	for (GLuint &texid : state.textureUnits)
	{
		if (texid == texture)
			texid = 0;
	}

	glDeleteTextures(1, &texture);
}

float OpenGL::setTextureFilter(graphics::Texture::Filter &f)
{
	GLint gmin, gmag;

	if (f.mipmap == Texture::FILTER_NONE)
	{
		if (f.min == Texture::FILTER_NEAREST)
			gmin = GL_NEAREST;
		else // f.min == Texture::FILTER_LINEAR
			gmin = GL_LINEAR;
	}
	else
	{
		if (f.min == Texture::FILTER_NEAREST && f.mipmap == Texture::FILTER_NEAREST)
			gmin = GL_NEAREST_MIPMAP_NEAREST;
		else if (f.min == Texture::FILTER_NEAREST && f.mipmap == Texture::FILTER_LINEAR)
			gmin = GL_NEAREST_MIPMAP_LINEAR;
		else if (f.min == Texture::FILTER_LINEAR && f.mipmap == Texture::FILTER_NEAREST)
			gmin = GL_LINEAR_MIPMAP_NEAREST;
		else if (f.min == Texture::FILTER_LINEAR && f.mipmap == Texture::FILTER_LINEAR)
			gmin = GL_LINEAR_MIPMAP_LINEAR;
		else
			gmin = GL_LINEAR;
	}


	switch (f.mag)
	{
	case Texture::FILTER_NEAREST:
		gmag = GL_NEAREST;
		break;
	case Texture::FILTER_LINEAR:
	default:
		gmag = GL_LINEAR;
		break;
	}

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gmin);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gmag);

	if (GLAD_EXT_texture_filter_anisotropic)
	{
		f.anisotropy = std::min(std::max(f.anisotropy, 1.0f), maxAnisotropy);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, f.anisotropy);
	}

	return f.anisotropy;
}

void OpenGL::setTextureWrap(const graphics::Texture::Wrap &w)
{
	auto glWrapMode = [](Texture::WrapMode wmode) -> GLint
	{
		switch (wmode)
		{
		case Texture::WRAP_CLAMP:
		default:
			return GL_CLAMP_TO_EDGE;
		case Texture::WRAP_REPEAT:
			return GL_REPEAT;
		case Texture::WRAP_MIRRORED_REPEAT:
			return GL_MIRRORED_REPEAT;
		}
	};

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, glWrapMode(w.s));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, glWrapMode(w.t));
}

int OpenGL::getMaxTextureSize() const
{
	return maxTextureSize;
}

int OpenGL::getMaxRenderTargets() const
{
	return maxRenderTargets;
}

void OpenGL::updateTextureMemorySize(size_t oldsize, size_t newsize)
{
	int64 memsize = (int64) stats.textureMemory + ((int64 )newsize -  (int64) oldsize);
	stats.textureMemory = (size_t) std::max(memsize, (int64) 0);
}

OpenGL::Vendor OpenGL::getVendor() const
{
	return vendor;
}

const char *OpenGL::debugSeverityString(GLenum severity)
{
	switch (severity)
	{
	case GL_DEBUG_SEVERITY_HIGH:
		return "high";
	case GL_DEBUG_SEVERITY_MEDIUM:
		return "medium";
	case GL_DEBUG_SEVERITY_LOW:
		return "low";
	default:
		break;
	}
	return "unknown";
}

const char *OpenGL::debugSourceString(GLenum source)
{
	switch (source)
	{
	case GL_DEBUG_SOURCE_API:
		return "API";
	case GL_DEBUG_SOURCE_WINDOW_SYSTEM:
		return "window";
	case GL_DEBUG_SOURCE_SHADER_COMPILER:
		return "shader";
	case GL_DEBUG_SOURCE_THIRD_PARTY:
		return "external";
	case GL_DEBUG_SOURCE_APPLICATION:
		return "LOVE";
	case GL_DEBUG_SOURCE_OTHER:
		return "other";
	default:
		break;
	}
	return "unknown";
}

const char *OpenGL::debugTypeString(GLenum type)
{
	switch (type)
	{
	case GL_DEBUG_TYPE_ERROR:
		return "error";
	case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
		return "deprecated behavior";
	case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
		return "undefined behavior";
	case GL_DEBUG_TYPE_PERFORMANCE:
		return "performance";
	case GL_DEBUG_TYPE_PORTABILITY:
		return "portability";
	case GL_DEBUG_TYPE_OTHER:
		return "other";
	default:
		break;
	}
	return "unknown";
}


// OpenGL class instance singleton.
OpenGL gl;

} // opengl
} // graphics
} // love
