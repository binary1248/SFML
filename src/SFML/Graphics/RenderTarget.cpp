////////////////////////////////////////////////////////////
//
// SFML - Simple and Fast Multimedia Library
// Copyright (C) 2007-2014 Laurent Gomila (laurent.gom@gmail.com)
//
// This software is provided 'as-is', without any express or implied warranty.
// In no event will the authors be held liable for any damages arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it freely,
// subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented;
//    you must not claim that you wrote the original software.
//    If you use this software in a product, an acknowledgment
//    in the product documentation would be appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such,
//    and must not be misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source distribution.
//
////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////
// Headers
////////////////////////////////////////////////////////////
#include <SFML/Graphics/RenderTarget.hpp>
#include <SFML/Graphics/Drawable.hpp>
#include <SFML/Graphics/Shader.hpp>
#include <SFML/Graphics/Texture.hpp>
#include <SFML/Graphics/VertexBuffer.hpp>
#include <SFML/Graphics/Light.hpp>
#include <SFML/Graphics/GLCheck.hpp>
#include <SFML/System/Err.hpp>
#include <sstream>


namespace sf
{
////////////////////////////////////////////////////////////
RenderTarget::RenderTarget() :
m_defaultView           (),
m_view                  (NULL),
m_cache                 (),
m_depthTest             (false),
m_clearDepth            (false),
m_defaultShader         (NULL),
m_currentNonLegacyShader(NULL),
m_lastNonLegacyShader   (NULL)
{
    m_cache.glStatesSet = false;
}


////////////////////////////////////////////////////////////
RenderTarget::~RenderTarget()
{
    delete m_defaultShader;
    delete m_view;
}


////////////////////////////////////////////////////////////
void RenderTarget::clear(const Color& color)
{
    if (activate(true))
    {
        glCheck(glClearColor(color.r / 255.f, color.g / 255.f, color.b / 255.f, color.a / 255.f));
        glCheck(glClear(GL_COLOR_BUFFER_BIT | (m_clearDepth ? GL_DEPTH_BUFFER_BIT : 0)));
    }
}


////////////////////////////////////////////////////////////
void RenderTarget::enableDepthTest(bool enable)
{
    m_depthTest = enable;

    if(enable)
    {
        glCheck(glEnable(GL_DEPTH_TEST));
        glCheck(glDepthFunc(GL_GEQUAL));
        glClearDepth(0.f);
        glDepthRangef(1.f, 0.f);
    }
    else
        glCheck(glDisable(GL_DEPTH_TEST));
}


////////////////////////////////////////////////////////////
const View& RenderTarget::getView() const
{
    return *m_view;
}


////////////////////////////////////////////////////////////
const View& RenderTarget::getDefaultView() const
{
    return m_defaultView;
}


////////////////////////////////////////////////////////////
IntRect RenderTarget::getViewport(const View& view) const
{
    float width  = static_cast<float>(getSize().x);
    float height = static_cast<float>(getSize().y);
    const FloatRect& viewport = view.getViewport();

    return IntRect(static_cast<int>(0.5f + width  * viewport.left),
                   static_cast<int>(0.5f + height * viewport.top),
                   static_cast<int>(width  * viewport.width),
                   static_cast<int>(height * viewport.height));
}


////////////////////////////////////////////////////////////
Vector2f RenderTarget::mapPixelToCoords(const Vector2i& point) const
{
    return mapPixelToCoords(point, getView());
}


////////////////////////////////////////////////////////////
Vector2f RenderTarget::mapPixelToCoords(const Vector2i& point, const View& view) const
{
    // First, convert from viewport coordinates to homogeneous coordinates
    Vector2f normalized;
    IntRect viewport = getViewport(view);
    normalized.x = -1.f + 2.f * (point.x - viewport.left) / viewport.width;
    normalized.y =  1.f - 2.f * (point.y - viewport.top)  / viewport.height;

    // Then transform by the inverse of the view matrix
    return view.getInverseTransform().transformPoint(normalized);
}

////////////////////////////////////////////////////////////
Vector2i RenderTarget::mapCoordsToPixel(const Vector3f& point) const
{
    return mapCoordsToPixel(point, getView());
}

////////////////////////////////////////////////////////////
Vector2i RenderTarget::mapCoordsToPixel(const Vector3f& point, const View& view) const
{
    // First, transform the point by the modelview and projection matrix
    Vector3f normalized = (view.getTransform() * view.getViewTransform()).transformPoint(point);

    // Then convert to viewport coordinates
    Vector2i pixel;
    IntRect viewport = getViewport(view);
    pixel.x = static_cast<int>(( normalized.x + 1.f) / 2.f * viewport.width  + viewport.left);
    pixel.y = static_cast<int>((-normalized.y + 1.f) / 2.f * viewport.height + viewport.top);

    return pixel;
}

////////////////////////////////////////////////////////////
void RenderTarget::draw(const Drawable& drawable, const RenderStates& states)
{
    drawable.draw(*this, states);
}

////////////////////////////////////////////////////////////
void RenderTarget::draw(const VertexBuffer& buffer, const RenderStates& states)
{
    // Nothing to draw?
    if (!buffer.getVertexCount())
        return;

    if (activate(true))
    {
        // First set the persistent OpenGL states if it's the very first call
        if (!m_cache.glStatesSet)
            resetGLStates();

        // Track if we need to set uniforms again for current shader
        bool shaderChanged = false;

        bool previousShaderWarnSetting = true;

        if (m_defaultShader)
        {
            // Non-legacy rendering, need to set uniforms
            if (states.shader)
            {
                m_currentNonLegacyShader = states.shader;
                previousShaderWarnSetting = states.shader->warnMissing(false);
            }
            else
                m_currentNonLegacyShader = m_defaultShader;

            shaderChanged = (m_currentNonLegacyShader != m_lastNonLegacyShader);
        }

        applyTransform(states.transform);

        // Apply the view
        if (shaderChanged || m_cache.viewChanged)
            applyCurrentView();

        // Apply the blend mode
        if (states.blendMode != m_cache.lastBlendMode)
            applyBlendMode(states.blendMode);

        // Apply the texture
        Uint64 textureId = states.texture ? states.texture->m_cacheId : 0;
        if (shaderChanged || (textureId != m_cache.lastTextureId))
            applyTexture(states.texture);

        // Apply the shader
        if (states.shader)
            applyShader(states.shader);
        else if (m_defaultShader)
            applyShader(m_defaultShader);

        // Apply the vertex buffer
        Uint64 vertexBufferId = buffer.m_cacheId;
        if (vertexBufferId != m_cache.lastVertexBufferId)
            applyVertexBuffer(&buffer);

        // Find the OpenGL primitive type
        static const GLenum modes[] = {GL_POINTS, GL_LINES, GL_LINE_STRIP, GL_TRIANGLES,
                                       GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN, GL_QUADS};
        GLenum mode = modes[buffer.getPrimitiveType()];

        // Setup the pointers to the vertices' components
        if (!m_defaultShader)
        {
            glCheck(glVertexPointer(3, GL_FLOAT, sizeof(Vertex), 0));
            glCheck(glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), reinterpret_cast<void*>(3 * sizeof(float))));
            glCheck(glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex), reinterpret_cast<void*>(3 * sizeof(float) + 4 * sizeof(char))));
            glCheck(glNormalPointer(GL_FLOAT, sizeof(Vertex), reinterpret_cast<void*>(5 * sizeof(float) + 4 * sizeof(char))));

            // Draw the primitives
            glCheck(glDrawArrays(mode, 0, buffer.getVertexCount()));
        }
        else
        {
            if (Light::isLightingEnabled())
            {
                const std::set<const Light*>& enabledLights = Light::getEnabledLights();
                m_currentNonLegacyShader->setParameter("sf_LightCount", static_cast<int>(enabledLights.size()));
                for (std::set<const Light*>::const_iterator i = enabledLights.begin(); i != enabledLights.end(); ++i)
                    (*i)->addToShader(*m_currentNonLegacyShader);

                m_currentNonLegacyShader->setParameter("sf_ViewerPosition", m_view->getPosition());
            }
            else
                m_currentNonLegacyShader->setParameter("sf_LightCount", 0);

            int vertexLocation   = m_currentNonLegacyShader->getVertexAttributeLocation("sf_Vertex");
            int colorLocation    = m_currentNonLegacyShader->getVertexAttributeLocation("sf_Color");
            int texCoordLocation = m_currentNonLegacyShader->getVertexAttributeLocation("sf_MultiTexCoord0");
            int normalLocation   = m_currentNonLegacyShader->getVertexAttributeLocation("sf_Normal");

            if (vertexLocation >= 0)
            {
                glCheck(glEnableVertexAttribArrayARB(vertexLocation));
                glCheck(glVertexAttribPointerARB(vertexLocation, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), 0));
            }

            if (colorLocation >= 0)
            {
                glCheck(glEnableVertexAttribArrayARB(colorLocation));
                glCheck(glVertexAttribPointerARB(colorLocation, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), reinterpret_cast<void*>(sizeof(Vector3f))));
            }

            if (texCoordLocation >= 0)
            {
                glCheck(glEnableVertexAttribArrayARB(texCoordLocation));
                glCheck(glVertexAttribPointerARB(texCoordLocation, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(sizeof(Vector3f) + sizeof(Color))));
            }

            if (normalLocation >= 0)
            {
                glCheck(glEnableVertexAttribArrayARB(normalLocation));
                glCheck(glVertexAttribPointerARB(normalLocation, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(sizeof(Vector3f) + sizeof(Color) + sizeof(Vector2f))));
            }

            // Draw the primitives
            glCheck(glDrawArrays(mode, 0, buffer.getVertexCount()));

            if (vertexLocation >= 0)
                glCheck(glDisableVertexAttribArrayARB(vertexLocation));

            if (colorLocation >= 0)
                glCheck(glDisableVertexAttribArrayARB(colorLocation));

            if (texCoordLocation >= 0)
                glCheck(glDisableVertexAttribArrayARB(texCoordLocation));

            if (normalLocation >= 0)
                glCheck(glDisableVertexAttribArrayARB(normalLocation));
        }

        // Unbind the shader, if any was bound in legacy mode
        if (states.shader && !m_defaultShader)
            applyShader(NULL);

        if (m_defaultShader)
        {
            if (states.shader)
                states.shader->warnMissing(previousShaderWarnSetting);

            m_lastNonLegacyShader = m_currentNonLegacyShader;
            m_currentNonLegacyShader = NULL;
        }
    }
}


////////////////////////////////////////////////////////////
void RenderTarget::draw(const Vertex* vertices, unsigned int vertexCount,
                        PrimitiveType type, const RenderStates& states)
{
    // Nothing to draw?
    if (!vertices || (vertexCount == 0))
        return;

    if (activate(true))
    {
        // First set the persistent OpenGL states if it's the very first call
        if (!m_cache.glStatesSet)
            resetGLStates();

        // Track if we need to set uniforms again for current shader
        bool shaderChanged = false;

        bool previousShaderWarnSetting = true;

        if (m_defaultShader)
        {
            // Non-legacy rendering, need to set uniforms
            if (states.shader)
            {
                m_currentNonLegacyShader = states.shader;
                previousShaderWarnSetting = states.shader->warnMissing(false);
            }
            else
                m_currentNonLegacyShader = m_defaultShader;

            shaderChanged = (m_currentNonLegacyShader != m_lastNonLegacyShader);
        }

        // Check if the vertex count is low enough so that we can pre-transform them
        bool useVertexCache = !m_defaultShader && (vertexCount <= StatesCache::VertexCacheSize) && states.useVertexCache;
        if (useVertexCache)
        {
            // Pre-transform the vertices and store them into the vertex cache
            for (unsigned int i = 0; i < vertexCount; ++i)
            {
                Vertex& vertex = m_cache.vertexCache[i];
                vertex.position = states.transform * vertices[i].position;
                vertex.color = vertices[i].color;
                vertex.texCoords = vertices[i].texCoords;
                vertex.normal = vertices[i].normal;
            }

            // Since vertices are transformed, we must use an identity transform to render them
            if (!m_cache.useVertexCache)
                applyTransform(Transform::Identity);
        }
        else
        {
            applyTransform(states.transform);
        }

        // Apply the view
        if (shaderChanged || m_cache.viewChanged)
            applyCurrentView();

        // Apply the blend mode
        if (states.blendMode != m_cache.lastBlendMode)
            applyBlendMode(states.blendMode);

        // Apply the texture
        Uint64 textureId = states.texture ? states.texture->m_cacheId : 0;
        if (shaderChanged || (textureId != m_cache.lastTextureId))
            applyTexture(states.texture);

        // Apply the shader
        if (states.shader)
            applyShader(states.shader);
        else if (m_defaultShader)
            applyShader(m_defaultShader);

        // Unbind any bound vertex buffer
        if (m_cache.lastVertexBufferId)
            applyVertexBuffer(NULL);

        // If we pre-transform the vertices, we must use our internal vertex cache
        if (useVertexCache)
        {
            // ... and if we already used it previously, we don't need to set the pointers again
            if (!m_cache.useVertexCache)
                vertices = m_cache.vertexCache;
            else
                vertices = NULL;
        }

        // Find the OpenGL primitive type
        static const GLenum modes[] = {GL_POINTS, GL_LINES, GL_LINE_STRIP, GL_TRIANGLES,
                                       GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN, GL_QUADS};
        GLenum mode = modes[type];

        // Setup the pointers to the vertices' components
        if (!m_defaultShader)
        {
            if (vertices)
            {
                const char* data = reinterpret_cast<const char*>(vertices);
                glCheck(glVertexPointer(3, GL_FLOAT, sizeof(Vertex), data + 0));
                glCheck(glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), data + 12));
                glCheck(glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex), data + 16));
                glCheck(glNormalPointer(GL_FLOAT, sizeof(Vertex), data + 24));
            }

            // Draw the primitives
            glCheck(glDrawArrays(mode, 0, vertexCount));
        }
        else
        {
            if (Light::isLightingEnabled())
            {
                const std::set<const Light*>& enabledLights = Light::getEnabledLights();
                m_currentNonLegacyShader->setParameter("sf_LightCount", static_cast<int>(enabledLights.size()));
                for (std::set<const Light*>::const_iterator i = enabledLights.begin(); i != enabledLights.end(); ++i)
                    (*i)->addToShader(*m_currentNonLegacyShader);

                m_currentNonLegacyShader->setParameter("sf_ViewerPosition", m_view->getPosition());
            }
            else
                m_currentNonLegacyShader->setParameter("sf_LightCount", 0);

            int vertexLocation   = m_currentNonLegacyShader->getVertexAttributeLocation("sf_Vertex");
            int colorLocation    = m_currentNonLegacyShader->getVertexAttributeLocation("sf_Color");
            int texCoordLocation = m_currentNonLegacyShader->getVertexAttributeLocation("sf_MultiTexCoord0");
            int normalLocation   = m_currentNonLegacyShader->getVertexAttributeLocation("sf_Normal");

            const char* data = reinterpret_cast<const char*>(vertices);

            if (vertexLocation >= 0)
            {
                glCheck(glEnableVertexAttribArrayARB(vertexLocation));
                glCheck(glVertexAttribPointerARB(vertexLocation, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), 0));
            }

            if (colorLocation >= 0)
            {
                glCheck(glEnableVertexAttribArrayARB(colorLocation));
                glCheck(glVertexAttribPointerARB(colorLocation, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), data + sizeof(Vector3f)));
            }

            if (texCoordLocation >= 0)
            {
                glCheck(glEnableVertexAttribArrayARB(texCoordLocation));
                glCheck(glVertexAttribPointerARB(texCoordLocation, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), data + sizeof(Vector3f) + sizeof(Color)));
            }

            if (normalLocation >= 0)
            {
                glCheck(glEnableVertexAttribArrayARB(normalLocation));
                glCheck(glVertexAttribPointerARB(normalLocation, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), data + sizeof(Vector3f) + sizeof(Color) + sizeof(Vector2f)));
            }

            // Draw the primitives
            glCheck(glDrawArrays(mode, 0, vertexCount));

            if (vertexLocation >= 0)
                glCheck(glDisableVertexAttribArrayARB(vertexLocation));

            if (colorLocation >= 0)
                glCheck(glDisableVertexAttribArrayARB(colorLocation));

            if (texCoordLocation >= 0)
                glCheck(glDisableVertexAttribArrayARB(texCoordLocation));

            if (normalLocation >= 0)
                glCheck(glDisableVertexAttribArrayARB(normalLocation));
        }

        // Unbind the shader, if any was bound in legacy mode
        if (states.shader && !m_defaultShader)
            applyShader(NULL);

        // Update the cache
        m_cache.useVertexCache = useVertexCache;

        if (m_defaultShader)
        {
            if (states.shader)
                states.shader->warnMissing(previousShaderWarnSetting);

            m_lastNonLegacyShader = m_currentNonLegacyShader;
            m_currentNonLegacyShader = NULL;
        }
    }
}


////////////////////////////////////////////////////////////
void RenderTarget::pushGLStates()
{
    if (activate(true))
    {
#ifdef SFML_DEBUG
        // make sure that the user didn't leave an unchecked OpenGL error
        GLenum error = glGetError();
        if (error != GL_NO_ERROR)
        {
            err() << "OpenGL error (" << error << ") detected in user code, "
                  << "you should check for errors with glGetError()"
                  << std::endl;
        }
#endif

        glCheck(glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS));
        glCheck(glPushAttrib(GL_ALL_ATTRIB_BITS));
        glCheck(glMatrixMode(GL_MODELVIEW));
        glCheck(glPushMatrix());
        glCheck(glMatrixMode(GL_PROJECTION));
        glCheck(glPushMatrix());
        glCheck(glMatrixMode(GL_TEXTURE));
        glCheck(glPushMatrix());
    }

    resetGLStates();
}


////////////////////////////////////////////////////////////
void RenderTarget::popGLStates()
{
    if (activate(true))
    {
        if (m_defaultShader)
            applyShader(NULL);

        glCheck(glMatrixMode(GL_PROJECTION));
        glCheck(glPopMatrix());
        glCheck(glMatrixMode(GL_MODELVIEW));
        glCheck(glPopMatrix());
        glCheck(glMatrixMode(GL_TEXTURE));
        glCheck(glPopMatrix());
        glCheck(glPopClientAttrib());
        glCheck(glPopAttrib());
    }
}


////////////////////////////////////////////////////////////
void RenderTarget::resetGLStates()
{
    if (activate(true))
    {
        // Make sure that GLEW is initialized
        priv::ensureGlewInit();

        // Define the default OpenGL states
        glCheck(glDisable(GL_LIGHTING));
        if(!m_depthTest)
            glCheck(glDisable(GL_DEPTH_TEST));
        glCheck(glDisable(GL_ALPHA_TEST));
        glCheck(glEnable(GL_CULL_FACE));
        glCheck(glEnable(GL_BLEND));

        if (!m_defaultShader)
        {
            glCheck(glEnable(GL_TEXTURE_2D));
            glCheck(glEnable(GL_COLOR_MATERIAL));
            glCheck(glEnable(GL_NORMALIZE));
            glCheck(glMatrixMode(GL_MODELVIEW));
            glCheck(glEnableClientState(GL_VERTEX_ARRAY));
            glCheck(glEnableClientState(GL_COLOR_ARRAY));
            glCheck(glEnableClientState(GL_TEXTURE_COORD_ARRAY));
            glCheck(glEnableClientState(GL_NORMAL_ARRAY));
        }

        glCheck(glPolygonMode(GL_FRONT_AND_BACK, GL_FILL));
        m_cache.glStatesSet = true;

        // Apply the default SFML states
        applyBlendMode(BlendAlpha);
        applyTransform(Transform::Identity);
        applyTexture(NULL);

        if (Shader::isAvailable())
        {
            if (!m_defaultShader)
                applyShader(NULL);
            else
                applyShader(m_defaultShader);
        }

        if (VertexBuffer::isAvailable())
            applyVertexBuffer(NULL);
        m_cache.useVertexCache = false;

        // Set the default view
        setView(m_defaultView);
    }
}


////////////////////////////////////////////////////////////
void RenderTarget::initialize()
{
    // Setup the default and current views
    m_defaultView.reset(FloatRect(0, 0, static_cast<float>(getSize().x), static_cast<float>(getSize().y)));

    delete m_view;
    m_view = new View(m_defaultView);

    // Set GL states only on first draw, so that we don't pollute user's states
    m_cache.glStatesSet = false;

    // Try to set up non-legacy pipeline if available
    setupNonLegacyPipeline();
}


////////////////////////////////////////////////////////////
void RenderTarget::applyCurrentView()
{
    // Set the viewport
    IntRect viewport = getViewport(*m_view);
    int top = getSize().y - (viewport.top + viewport.height);
    glCheck(glViewport(viewport.left, top, viewport.width, viewport.height));

    if (m_defaultShader)
    {
        const Shader* shader = NULL;

        if (m_currentNonLegacyShader)
            shader = m_currentNonLegacyShader;
        else
            shader = m_defaultShader;

        shader->setParameter("sf_ProjectionMatrix", m_view->getTransform());
    }
    else
    {
        // Set the projection matrix
        glCheck(glMatrixMode(GL_PROJECTION));
        glCheck(glLoadMatrixf(m_view->getTransform().getMatrix()));

        // Go back to model-view mode
        glCheck(glMatrixMode(GL_MODELVIEW));
    }

    m_cache.viewChanged = false;
}


////////////////////////////////////////////////////////////
void RenderTarget::applyBlendMode(BlendMode mode)
{
    switch (mode)
    {
        // glBlendFuncSeparateEXT is used when available to avoid an incorrect alpha value when the target
        // is a RenderTexture -- in this case the alpha value must be written directly to the target buffer

        // Alpha blending
        default :
        case BlendAlpha :
            if (GLEW_EXT_blend_func_separate)
                glCheck(glBlendFuncSeparateEXT(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA));
            else
                glCheck(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
            break;

        // Additive blending
        case BlendAdd :
            if (GLEW_EXT_blend_func_separate)
                glCheck(glBlendFuncSeparateEXT(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE));
            else
                glCheck(glBlendFunc(GL_SRC_ALPHA, GL_ONE));
            break;

        // Multiplicative blending
        case BlendMultiply :
            glCheck(glBlendFunc(GL_DST_COLOR, GL_ZERO));
            break;

        // No blending
        case BlendNone :
            glCheck(glBlendFunc(GL_ONE, GL_ZERO));
            break;
    }

    m_cache.lastBlendMode = mode;
}


////////////////////////////////////////////////////////////
void RenderTarget::applyTransform(const Transform& transform)
{
    if (m_defaultShader)
    {
        const Shader* shader = NULL;

        if (m_currentNonLegacyShader)
            shader = m_currentNonLegacyShader;
        else
            shader = m_defaultShader;

        shader->setParameter("sf_ViewMatrix", m_view->getViewTransform());
        shader->setParameter("sf_ModelMatrix", transform);

        const float* modelMatrix = transform.getMatrix();
        Transform normalMatrix(modelMatrix[0], modelMatrix[4], modelMatrix[8],  0.f,
                               modelMatrix[1], modelMatrix[5], modelMatrix[9],  0.f,
                               modelMatrix[2], modelMatrix[6], modelMatrix[10], 0.f,
                               0.f,            0.f,            0.f,             1.f);

        if (sf::Light::isLightingEnabled())
            shader->setParameter("sf_NormalMatrix", normalMatrix.getInverse().getTranspose());
    }
    else
        // No need to call glMatrixMode(GL_MODELVIEW), it is always the
        // current mode (for optimization purpose, since it's the most used)
        glCheck(glLoadMatrixf((m_view->getViewTransform() * transform).getMatrix()));
}


////////////////////////////////////////////////////////////
void RenderTarget::applyTexture(const Texture* texture)
{
    if (m_defaultShader)
    {
        const Shader* shader = NULL;

        if (m_currentNonLegacyShader)
            shader = m_currentNonLegacyShader;
        else
            shader = m_defaultShader;

        float xScale = 1.f;
        float yScale = 1.f;
        float yFlip  = 0.f;

        if (texture)
        {
            // Setup scale factors that convert the range [0 .. size] to [0 .. 1]
            xScale = 1.f / texture->m_actualSize.x;
            yScale = 1.f / texture->m_actualSize.y;

            // If pixels are flipped we must invert the Y axis
            if (texture->m_pixelsFlipped)
            {
                yScale = -yScale;
                yFlip = static_cast<float>(texture->m_size.y) / texture->m_actualSize.y;
            }

            Transform textureMatrix(xScale, 0.f,    0.f, 0.f,
                                    0.f,    yScale, 0.f, yFlip,
                                    0.f,    0.f,    1.f, 0.f,
                                    0.f,    0.f,    0.f, 1.f);

            shader->setParameter("sf_TextureMatrix", textureMatrix);
            shader->setParameter("sf_Texture0", *texture);
            shader->setParameter("sf_TextureEnabled", 1);
        }
        else
            shader->setParameter("sf_TextureEnabled", 0);
    }
    else
        Texture::bind(texture, Texture::Pixels);

    m_cache.lastTextureId = texture ? texture->m_cacheId : 0;
}


////////////////////////////////////////////////////////////
void RenderTarget::applyShader(const Shader* shader)
{
    Shader::bind(shader);
}


////////////////////////////////////////////////////////////
void RenderTarget::applyVertexBuffer(const VertexBuffer* buffer)
{
    VertexBuffer::bind(buffer);

    m_cache.lastVertexBufferId = buffer ? buffer->m_cacheId : 0;
}


////////////////////////////////////////////////////////////
void RenderTarget::setupNonLegacyPipeline()
{
    // Setup the default shader if non-legacy rendering is supported
    delete m_defaultShader;
    m_defaultShader = NULL;

    // Check if our shader lighting implementation is supported
    if (!Light::hasShaderLighting())
        return;

    double versionNumber = 0.0;
    std::istringstream versionStringStream(Shader::getSupportedVersion());
    versionStringStream >> versionNumber;

// Disable non-legacy pipeline if requested
#if defined(SFML_LEGACY_GL)
    versionNumber = 0.0;
#endif

    // This will only succeed if the supported version is not GLSL ES
    if (versionNumber > 1.29)
    {
        m_defaultShader = new Shader;

        std::stringstream vertexShaderSource;
        vertexShaderSource << "#version 130\n"
                              "\n"
                              "// Uniforms\n"
                              "uniform mat4 sf_ModelMatrix;\n"
                              "uniform mat4 sf_ViewMatrix;\n"
                              "uniform mat4 sf_ProjectionMatrix;\n"
                              "uniform mat4 sf_TextureMatrix;\n"
                              "uniform int sf_TextureEnabled;\n"
                              "uniform int sf_LightCount;\n"
                              "\n"
                              "// Vertex attributes\n"
                              "in vec3 sf_Vertex;\n"
                              "in vec4 sf_Color;\n"
                              "in vec2 sf_MultiTexCoord0;\n"
                              "in vec3 sf_Normal;\n"
                              "\n"
                              "// Vertex shader outputs\n"
                              "out vec4 sf_FrontColor;\n"
                              "out vec2 sf_TexCoord0;\n"
                              "out vec3 sf_FragCoord;\n"
                              "out vec3 sf_FragNormal;\n"
                              "\n"
                              "void main()\n"
                              "{\n"
                              "    // Vertex position\n"
                              "    gl_Position = sf_ProjectionMatrix * sf_ViewMatrix * sf_ModelMatrix * vec4(sf_Vertex, 1.0);\n"
                              "\n"
                              "    // Vertex color\n"
                              "    sf_FrontColor = sf_Color;\n"
                              "\n"
                              "    // Texture data\n"
                              "    if (sf_TextureEnabled == 1)\n"
                              "        sf_TexCoord0 = (sf_TextureMatrix * vec4(sf_MultiTexCoord0, 0.0, 1.0)).st;\n"
                              "\n"
                              "    // Lighting data\n"
                              "    if (sf_LightCount > 0)\n"
                              "    {\n"
                              "        sf_FragNormal = sf_Normal;\n"
                              "        sf_FragCoord = sf_Vertex;\n"
                              "    }\n"
                              "}\n";

        std::stringstream fragmentShaderSource;
        fragmentShaderSource << "#version 130\n"
                                "\n"
                                "// Light structure\n"
                                "struct Light\n"
                                "{\n"
                                "    vec4  color;\n"
                                "    vec4  positionDirection;\n"
                                "    float ambientIntensity;\n"
                                "    float diffuseIntensity;\n"
                                "    float specularIntensity;\n"
                                "    float constantAttenuation;\n"
                                "    float linearAttenuation;\n"
                                "    float quadraticAttenuation;\n"
                                "};\n"
                                "\n"
                                "// Uniforms\n"
                                "uniform mat4 sf_ModelMatrix;\n"
                                "uniform mat4 sf_NormalMatrix;\n"
                                "uniform sampler2D sf_Texture0;\n"
                                "uniform int sf_TextureEnabled;\n"
                                "uniform Light sf_Lights[" << Light::getMaximumLights() << "];\n"
                                "uniform int sf_LightCount;\n"
                                "uniform vec3 sf_ViewerPosition;\n"
                                "\n"
                                "// Fragment attributes\n"
                                "in vec4 sf_FrontColor;\n"
                                "in vec2 sf_TexCoord0;\n"
                                "in vec3 sf_FragCoord;\n"
                                "in vec3 sf_FragNormal;\n"
                                "\n"
                                "// Fragment shader outputs\n"
                                "out vec4 sf_FragColor;\n"
                                "\n"
                                "vec4 computeLighting()\n"
                                "{\n"
                                "    // Early return in case lighting disabled\n"
                                "    if (sf_LightCount == 0)\n"
                                "        return vec4(1.0, 1.0, 1.0, 1.0);\n"
                                "\n"
                                "    // TODO: Implement way to manipulate materials\n"
                                "    const float materialShininess = 1.0;\n"
                                "    const vec4 materialSpecularColor = vec4(0.0001, 0.0001, 0.0001, 1.0);\n"
                                "\n"
                                "    vec3 fragmentNormal = normalize((sf_NormalMatrix * vec4(sf_FragNormal, 1.0)).xyz);\n"
                                "    vec3 fragmentWorldPosition = vec3(sf_ModelMatrix * vec4(sf_FragCoord, 1.0));\n"
                                "    vec3 fragmentDistanceToViewer = normalize(sf_ViewerPosition - fragmentWorldPosition);"
                                "\n"
                                "    vec4 totalIntensity = vec4(1.0, 1.0, 1.0, 1.0);\n"
                                "    if (sf_LightCount > 0)\n"
                                "        totalIntensity = vec4(0.0, 0.0, 0.0, 0.0);\n"
                                "    for (int index = 0; index < sf_LightCount; ++index)\n"
                                "    {\n"
                                "        vec3 fragmentToLightDirection = normalize(-sf_Lights[index].positionDirection.xyz);\n"
                                "        float attenuationFactor = 1.0;"
                                "\n"
                                "        if (sf_Lights[index].positionDirection.w > 0.0)\n"
                                "        {\n"
                                "            fragmentToLightDirection = normalize(sf_Lights[index].positionDirection.xyz - fragmentWorldPosition);\n"
                                "            float rayLength = length(sf_Lights[index].positionDirection.xyz - fragmentWorldPosition);"
                                "            attenuationFactor = sf_Lights[index].constantAttenuation +\n"
                                "                                sf_Lights[index].linearAttenuation * rayLength +\n"
                                "                                sf_Lights[index].quadraticAttenuation * rayLength * rayLength;\n"
                                "        }\n"
                                "\n"
                                "        vec4 ambientIntensity = sf_Lights[index].color * sf_Lights[index].ambientIntensity;\n"
                                "\n"
                                "        float diffuseCoefficient = max(0.0, dot(fragmentNormal, fragmentToLightDirection));\n"
                                "        vec4 diffuseIntensity = sf_Lights[index].color * sf_Lights[index].diffuseIntensity * diffuseCoefficient;\n"
                                "\n"
                                "        float specularCoefficient = 0.0;\n"
                                "        if(diffuseCoefficient > 0.0)"
                                "            specularCoefficient = pow(max(0.0, dot(fragmentDistanceToViewer, reflect(-fragmentToLightDirection, fragmentNormal))), materialShininess);"
                                "        vec4 specularIntensity = specularCoefficient * materialSpecularColor * sf_Lights[index].color * sf_Lights[index].specularIntensity;"
                                "\n"
                                "        totalIntensity += ambientIntensity + (diffuseIntensity + specularIntensity) / attenuationFactor;\n"
                                "    }\n"
                                "\n"
                                "    return vec4(totalIntensity.rgb, 1.0);\n"
                                "}\n"
                                "\n"
                                "vec4 computeTexture()\n"
                                "{\n"
                                "    if (sf_TextureEnabled == 0)\n"
                                "        return vec4(1.0, 1.0, 1.0, 1.0);\n"
                                "\n"
                                "    return texture2D(sf_Texture0, sf_TexCoord0);\n"
                                "}\n"
                                "\n"
                                "void main()\n"
                                "{\n"
                                "    // Fragment color\n"
                                "    sf_FragColor = sf_FrontColor * computeTexture() * computeLighting();\n"
                                "}\n";

        if (!m_defaultShader->loadFromMemory(vertexShaderSource.str(), fragmentShaderSource.str()))
        {
            err() << "Compiling default shader failed. Falling back to legacy pipeline..." << std::endl;
            delete m_defaultShader;
            m_defaultShader = NULL;
        }
    }
}

} // namespace sf


////////////////////////////////////////////////////////////
// Render states caching strategies
//
// * View
//   If SetView was called since last draw, the projection
//   matrix is updated. We don't need more, the view doesn't
//   change frequently.
//
// * Transform
//   The transform matrix is usually expensive because each
//   entity will most likely use a different transform. This can
//   lead, in worst case, to changing it every 4 vertices.
//   To avoid that, when the vertex count is low enough, we
//   pre-transform them and therefore use an identity transform
//   to render them.
//
// * Blending mode
//   It's a simple integral value, so we can easily check
//   whether the value to apply is the same as before or not.
//
// * Texture
//   Storing the pointer or OpenGL ID of the last used texture
//   is not enough; if the sf::Texture instance is destroyed,
//   both the pointer and the OpenGL ID might be recycled in
//   a new texture instance. We need to use our own unique
//   identifier system to ensure consistent caching.
//
// * Shader
//   Shaders are very hard to optimize, because they have
//   parameters that can be hard (if not impossible) to track,
//   like matrices or textures. The only optimization that we
//   do is that we avoid setting a null shader if there was
//   already none for the previous draw.
//
////////////////////////////////////////////////////////////
