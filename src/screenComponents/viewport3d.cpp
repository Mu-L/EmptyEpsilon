#include <graphics/opengl.h>
#include <ecs/query.h>

#include "main.h"
#include "playerInfo.h"
#include "gameGlobalInfo.h"
#include "viewport3d.h"
#include "shaderManager.h"
#include "soundManager.h"
#include "textureManager.h"
#include "random.h"
#include "preferenceManager.h"
#include "particleEffect.h"
#include "glObjects.h"
#include "shaderRegistry.h"
#include "components/collision.h"
#include "components/target.h"
#include "components/hull.h"
#include "components/rendering.h"
#include "components/impulse.h"
#include "components/name.h"
#include "components/zone.h"
#include "systems/rendering.h"
#include "math/centerOfMass.h"

#include <glm/glm.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/type_ptr.hpp>


static std::unordered_map<string, std::unique_ptr<gl::CubemapTexture>> skybox_textures;


GuiViewport3D::GuiViewport3D(GuiContainer* owner, string id)
: GuiElement(owner, id)
{
    show_callsigns = false;
    show_headings = false;
    show_spacedust = false;

    // Load up our starbox into a cubemap.
    // Setup shader.
    starbox_shader = ShaderManager::getShader("shaders/starbox");
    starbox_shader->bind();
    starbox_uniforms[static_cast<size_t>(Uniforms::Projection)] = starbox_shader->getUniformLocation("projection");
    starbox_uniforms[static_cast<size_t>(Uniforms::View)] = starbox_shader->getUniformLocation("view");
    starbox_uniforms[static_cast<size_t>(Uniforms::LocalBox)] = starbox_shader->getUniformLocation("local_starbox");
    starbox_uniforms[static_cast<size_t>(Uniforms::GlobalBox)] = starbox_shader->getUniformLocation("global_starbox");
    starbox_uniforms[static_cast<size_t>(Uniforms::BoxLerp)] = starbox_shader->getUniformLocation("starbox_lerp");

    starbox_vertex_attributes[static_cast<size_t>(VertexAttributes::Position)] = starbox_shader->getAttributeLocation("position");

    // Load up the ebo and vbo for the cube.
    /*   
           .2------6
         .' |    .'|
        3---+--7'  |
        |   |  |   |
        |  .0--+---4
        |.'    | .'
        1------5'
    */
    std::array<glm::vec3, 8> positions{
        // Left face
        glm::vec3{-1.f, -1.f, -1.f}, // 0
        glm::vec3{-1.f, -1.f, 1.f},  // 1
        glm::vec3{-1.f, 1.f, -1.f},  // 2
        glm::vec3{-1.f, 1.f, 1.f},   // 3

        // Right face
        glm::vec3{1.f, -1.f, -1.f},  // 4
        glm::vec3{1.f, -1.f, 1.f},   // 5
        glm::vec3{1.f, 1.f, -1.f},   // 6
        glm::vec3{1.f, 1.f, 1.f},    // 7
    };

    constexpr std::array<uint16_t, 6 * 6> elements{
        2, 6, 4, 4, 0, 2, // Back
        3, 2, 0, 0, 1, 3, // Left
        6, 7, 5, 5, 4, 6, // Right
        7, 3, 1, 1, 5, 7, // Front
        6, 2, 3, 3, 7, 6, // Top
        0, 4, 5, 5, 1, 0, // Bottom
    };

    // Upload to GPU.
    glBindBuffer(GL_ARRAY_BUFFER, starbox_buffers[static_cast<size_t>(Buffers::Vertex)]);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, starbox_buffers[static_cast<size_t>(Buffers::Element)]);

    glBufferData(GL_ARRAY_BUFFER, positions.size() * sizeof(glm::vec3), positions.data(), GL_STATIC_DRAW);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, elements.size() * sizeof(uint16_t), elements.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, GL_NONE);
    // Setup spacedust
    spacedust_shader = ShaderManager::getShader("shaders/spacedust");
    spacedust_shader->bind();
    spacedust_uniforms[static_cast<size_t>(Uniforms::Projection)] = spacedust_shader->getUniformLocation("projection");
    spacedust_uniforms[static_cast<size_t>(Uniforms::View)] = spacedust_shader->getUniformLocation("view");
    spacedust_uniforms[static_cast<size_t>(Uniforms::Rotation)] = spacedust_shader->getUniformLocation("rotation");

    spacedust_vertex_attributes[static_cast<size_t>(VertexAttributes::Position)] = spacedust_shader->getAttributeLocation("position");
    spacedust_vertex_attributes[static_cast<size_t>(VertexAttributes::Sign)] = spacedust_shader->getAttributeLocation("sign_value");

    // Reserve our GPU buffer.
    // Each dust particle consist of:
    // - a worldpace position (Vector3f)
    // - a sign value (single byte, passed as float).
    // Both "arrays" are maintained separate:
    // the signs are stable (they just tell us which "end" of the line we're on)
    // The positions will get updated more frequently.
    // It means each particle occupies 2*16B (assuming tight packing)
    glBindBuffer(GL_ARRAY_BUFFER, spacedust_buffer[0]);
    glBufferData(GL_ARRAY_BUFFER, 2 * spacedust_particle_count * (sizeof(glm::vec3) + sizeof(float)), nullptr, GL_DYNAMIC_DRAW);

    // Generate and update the alternating vertices signs.
    std::array<float, 2 * spacedust_particle_count> signs;
    
    for (auto n = 0U; n < signs.size(); n += 2)
    {
        signs[n] = -1.f;
        signs[n + 1] = 1.f;
    }

    // Update sign parts.
    glBufferSubData(GL_ARRAY_BUFFER, 2 * spacedust_particle_count * sizeof(glm::vec3), signs.size() * sizeof(float), signs.data());
    {
        // zero out positions.
        const std::vector<glm::vec3> zeroed_positions(2 * spacedust_particle_count);
        glBufferSubData(GL_ARRAY_BUFFER, 0, 2 * spacedust_particle_count * sizeof(glm::vec3), zeroed_positions.data());
    }
    glBindBuffer(GL_ARRAY_BUFFER, GL_NONE);
}

void GuiViewport3D::onDraw(sp::RenderTarget& renderer)
{
    if (rect.size.x == 0.f)
    {
        // The GUI ticks before Updatables.
        // When the 3D screen is on the side of a station,
        // and the window is resized in a way that will hide the main screen,
        // this leaves a *one frame* gap where the 3D gui element is 'visible' but will try to render
        // with a computed 0-width rect.
        // Since some gl calls don't really like an empty viewport, just ignore the draw.
        return;
    }
    renderer.finish();
   
    if (auto transform = my_spaceship.getComponent<sp::Transform>())
        soundManager->setListenerPosition(transform->getPosition(), transform->getRotation());
    else
        soundManager->setListenerPosition(glm::vec2(camera_position.x, camera_position.y), camera_yaw);
    
    glActiveTexture(GL_TEXTURE0);

    float camera_fov = PreferencesManager::get("main_screen_camera_fov", "60").toFloat();
    {
        auto p0 = renderer.virtualToPixelPosition(rect.position);
        auto p1 = renderer.virtualToPixelPosition(rect.position + rect.size);
        glViewport(p0.x, renderer.getPhysicalSize().y - p1.y, p1.x - p0.x, p1.y - p0.y);
    }
    if (GLAD_GL_ES_VERSION_2_0)
        glClearDepthf(1.f);
    else
        glClearDepth(1.f);

    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    projection_matrix = glm::perspective(glm::radians(camera_fov), rect.size.x / rect.size.y, 1.f, 25000.f);

    // OpenGL standard: X across (left-to-right), Y up, Z "towards".
    view_matrix = glm::rotate(glm::identity<glm::mat4>(), glm::radians(90.0f), {1.f, 0.f, 0.f}); // -> X across (l-t-r), Y "towards", Z down 
    view_matrix = glm::scale(view_matrix, {1.f,1.f,-1.f});  // -> X across (l-t-r), Y "towards", Z up
    view_matrix = glm::rotate(view_matrix, glm::radians(-camera_pitch), {1.f, 0.f, 0.f});
    view_matrix = glm::rotate(view_matrix, glm::radians(-(camera_yaw + 90.f)), {0.f, 0.f, 1.f});

    // Translate camera
    view_matrix = glm::translate(view_matrix, -camera_position);

    // Draw starbox.
    glDepthMask(GL_FALSE);
    {
        starbox_shader->bind();
        glUniform1f(starbox_shader->getUniformLocation("scale"), 100.0f);

        string skybox_name = "skybox/default";
        if (gameGlobalInfo)
            skybox_name = "skybox/" + gameGlobalInfo->default_skybox;

        string local_skybox_name = skybox_name;
        float local_skybox_factor = 0.0f;
        for(auto [entity, zone, t] : sp::ecs::Query<Zone, sp::Transform>()) {
            if (zone.skybox.empty()) continue;

            auto pos = t.getPosition() - glm::vec2(camera_position.x, camera_position.y);
            if (insidePolygon(zone.outline, pos)) {
                local_skybox_name = "skybox/" + zone.skybox;
                if (zone.skybox_fade_distance <= 0)
                    local_skybox_factor = 1.0;
                else
                    local_skybox_factor = std::clamp(distanceToEdge(zone.outline, pos) / zone.skybox_fade_distance, 0.0f, 1.0f);
                break;
            }
        }

        auto skybox_texture = skybox_textures[skybox_name].get();
        if (!skybox_texture) {
            skybox_textures[skybox_name] = std::make_unique<gl::CubemapTexture>(skybox_name);
            skybox_texture = skybox_textures[skybox_name].get();
        }
        auto local_skybox_texture = skybox_textures[local_skybox_name].get();
        if (!local_skybox_texture) {
            skybox_textures[local_skybox_name] = std::make_unique<gl::CubemapTexture>(local_skybox_name);
            local_skybox_texture = skybox_textures[local_skybox_name].get();
        }

        // Setup shared state (uniforms)
        glUniform1i(starbox_uniforms[static_cast<size_t>(Uniforms::GlobalBox)], 0);
        glActiveTexture(GL_TEXTURE0);
        skybox_texture->bind();

        glUniform1i(starbox_uniforms[static_cast<size_t>(Uniforms::LocalBox)], 1);
        glActiveTexture(GL_TEXTURE1);
        local_skybox_texture->bind();

        glUniform1f(starbox_uniforms[static_cast<size_t>(Uniforms::BoxLerp)], local_skybox_factor);
        
        // Uniform
        // Upload matrices (only float 4x4 supported in es2)
        glUniformMatrix4fv(starbox_uniforms[static_cast<size_t>(Uniforms::Projection)], 1, GL_FALSE, glm::value_ptr(projection_matrix));
        glUniformMatrix4fv(starbox_uniforms[static_cast<size_t>(Uniforms::View)], 1, GL_FALSE, glm::value_ptr(view_matrix));
        
        // Bind our cube
        {
            gl::ScopedVertexAttribArray positions(starbox_vertex_attributes[static_cast<size_t>(VertexAttributes::Position)]);
            glBindBuffer(GL_ARRAY_BUFFER, starbox_buffers[static_cast<size_t>(Buffers::Vertex)]);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, starbox_buffers[static_cast<size_t>(Buffers::Element)]);

            // Vertex attributes.
            glVertexAttribPointer(positions.get(), 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (GLvoid*)0);

            glDrawElements(GL_TRIANGLES, 6 * 6, GL_UNSIGNED_SHORT, (GLvoid*)0);

            // Cleanup
            glBindBuffer(GL_ARRAY_BUFFER, GL_NONE);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, GL_NONE);
        }

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_CUBE_MAP, GL_NONE);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, GL_NONE);
    }
    glDepthMask(GL_TRUE);

    // Emit engine particles.
    for(auto [entity, ee, transform, impulse] : sp::ecs::Query<EngineEmitter, sp::Transform, ImpulseEngine>()) {
        if (impulse.actual != 0.0f) {
            float engine_scale = std::abs(impulse.actual);
            if (engine->getElapsedTime() - ee.last_engine_particle_time > 0.1f)
            {
                for(auto ed : ee.emitters)
                {
                    glm::vec3 offset = ed.position;
                    glm::vec2 pos2d = transform.getPosition() + rotateVec2(glm::vec2(offset.x, offset.y), transform.getRotation());
                    glm::vec3 color = ed.color;
                    glm::vec3 pos3d = glm::vec3(pos2d.x, pos2d.y, offset.z);
                    float scale = ed.scale * engine_scale;
                    ParticleEngine::spawn(pos3d, pos3d, color, color, scale, 0.0, 5.0);
                }
                ee.last_engine_particle_time = engine->getElapsedTime();
            }
        }
    }

    // Update view matrix in shaders.
    ShaderRegistry::updateProjectionView({}, view_matrix);

    RenderSystem render_system;
    render_system.render3D(rect.size.x / rect.size.y, camera_fov);

    ParticleEngine::render(projection_matrix, view_matrix);

    if (show_spacedust && my_spaceship)
    {
        auto transform = my_spaceship.getComponent<sp::Transform>();
        auto physics = my_spaceship.getComponent<sp::Physics>();
        static std::vector<glm::vec3> space_dust(2 * spacedust_particle_count);
        
        glm::vec2 dust_vector = physics ? (physics->getVelocity() / 100.f) : glm::vec2{0, 0};
        glm::vec3 dust_center = transform ? glm::vec3(transform->getPosition().x, transform->getPosition().y, 0.f) : camera_position;

        constexpr float maxDustDist = 500.f;
        constexpr float minDustDist = 100.f;
        
        bool update_required = false; // Do we need to update the GPU buffer?

        for (auto n = 0U; n < space_dust.size(); n += 2)
        {
            //
            auto delta = space_dust[n] - dust_center;
            if (glm::length2(delta) > maxDustDist*maxDustDist || glm::length2(delta) < minDustDist*minDustDist)
            {
                update_required = true;
                space_dust[n] = dust_center + glm::vec3(random(-maxDustDist, maxDustDist), random(-maxDustDist, maxDustDist), random(-maxDustDist, maxDustDist));
                space_dust[n + 1] = space_dust[n];
            }
        }

        spacedust_shader->bind();

        // Upload matrices (only float 4x4 supported in es2)
        glUniformMatrix4fv(spacedust_uniforms[static_cast<size_t>(Uniforms::Projection)], 1, GL_FALSE, glm::value_ptr(projection_matrix));
        glUniformMatrix4fv(spacedust_uniforms[static_cast<size_t>(Uniforms::View)], 1, GL_FALSE, glm::value_ptr(view_matrix));

        // Ship information for flying particles
        glUniform2f(spacedust_shader->getUniformLocation("velocity"), dust_vector.x, dust_vector.y);
        
        {
            gl::ScopedVertexAttribArray positions(spacedust_vertex_attributes[static_cast<size_t>(VertexAttributes::Position)]);
            gl::ScopedVertexAttribArray signs(spacedust_vertex_attributes[static_cast<size_t>(VertexAttributes::Sign)]);
            glBindBuffer(GL_ARRAY_BUFFER, spacedust_buffer[0]);
            
            if (update_required)
            {
                glBufferSubData(GL_ARRAY_BUFFER, 0, space_dust.size() * sizeof(glm::vec3), space_dust.data());
            }
            glVertexAttribPointer(positions.get(), 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (GLvoid*)0);
            glVertexAttribPointer(signs.get(), 1, GL_FLOAT, GL_FALSE, 0, (GLvoid*)(2 * spacedust_particle_count * sizeof(glm::vec3)));
            
            glDrawArrays(GL_LINES, 0, 2 * spacedust_particle_count);
            glBindBuffer(GL_ARRAY_BUFFER, GL_NONE);
        }
    }

    auto target_comp = my_spaceship.getComponent<Target>();
    if (target_comp && target_comp->entity)
    {
        ShaderRegistry::ScopedShader billboard(ShaderRegistry::Shaders::Billboard);

        glDisable(GL_DEPTH_TEST);
        glm::mat4 model_matrix = glm::identity<glm::mat4>();
        if (auto transform = target_comp->entity.getComponent<sp::Transform>())
            model_matrix = glm::translate(model_matrix, glm::vec3(transform->getPosition(), 0.f));

        textureManager.getTexture("redicule2.png")->bind();
        glUniformMatrix4fv(billboard.get().uniform(ShaderRegistry::Uniforms::Model), 1, GL_FALSE, glm::value_ptr(model_matrix));
        float radius = 300.0f;
        if (auto physics = target_comp->entity.getComponent<sp::Physics>())
            radius = physics->getSize().x;
        glUniform4f(billboard.get().uniform(ShaderRegistry::Uniforms::Color), .5f, .5f, .5f, radius * 2.5f);
        {
            gl::ScopedVertexAttribArray positions(billboard.get().attribute(ShaderRegistry::Attributes::Position));
            gl::ScopedVertexAttribArray texcoords(billboard.get().attribute(ShaderRegistry::Attributes::Texcoords));
            auto vertices = {
                0.f, 0.f, 0.f,
                0.f, 0.f, 0.f,
                0.f, 0.f, 0.f,
                0.f, 0.f, 0.f
            };
            glVertexAttribPointer(positions.get(), 3, GL_FLOAT, GL_FALSE, 0, (GLvoid*)vertices.begin());
            auto coords = {
                0.f, 1.f,
                1.f, 1.f,
                1.f, 0.f,
                0.f, 0.f
            };
            glVertexAttribPointer(texcoords.get(), 2, GL_FLOAT, GL_FALSE, 0, (GLvoid*)coords.begin());
            std::initializer_list<uint16_t> indices{ 0, 2, 1, 0, 3, 2 };
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, std::begin(indices));
        }
    }

    glDepthMask(true);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
/*
#ifdef DEBUG
    glDisable(GL_DEPTH_TEST);
    
    {
        ShaderRegistry::ScopedShader debug_shader(ShaderRegistry::Shaders::BasicColor);
        // Common state: color, projection matrix.
        glUniform4f(debug_shader.get().uniform(ShaderRegistry::Uniforms::Color), 1.f, 1.f, 1.f, 1.f);

        std::array<float, 16> matrix;
        glUniformMatrix4fv(debug_shader.get().uniform(ShaderRegistry::Uniforms::Projection), 1, GL_FALSE, glm::value_ptr(projection_matrix));

        std::vector<glm::vec3> points;
        gl::ScopedVertexAttribArray positions(debug_shader.get().attribute(ShaderRegistry::Attributes::Position));

        foreach(SpaceObject, obj, space_object_list)
        {
            glPushMatrix();
            glTranslatef(-camera_position.x, -camera_position.y, -camera_position.z);
            glTranslatef(obj->getPosition().x, obj->getPosition().y, 0);
            glRotatef(obj->getRotation(), 0, 0, 1);

            glGetFloatv(GL_MODELVIEW_MATRIX, matrix.data());
            glUniformMatrix4fv(debug_shader.get().uniform(ShaderRegistry::Uniforms::ModelView), 1, GL_FALSE, matrix.data());

            auto collisionShape = obj->getCollisionShape();

            if (collisionShape.size() > points.size())
            {
                points.resize(collisionShape.size());
                glVertexAttribPointer(positions.get(), 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), points.data());
            }

            for (unsigned int n = 0; n < collisionShape.size(); n++)
                points[n] = glm::vec3(collisionShape[n].x, collisionShape[n].y, 0.f);
            
            glDrawArrays(GL_LINE_LOOP, 0, collisionShape.size());
            glPopMatrix();
        }
    }
#endif
*/
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (show_callsigns)
    {
        for(auto [entity, callsign, transform] : sp::ecs::Query<CallSign, sp::Transform>())
        {
            if (entity == my_spaceship)
                continue;
            float radius = 300.0f;
            if (auto physics = entity.getComponent<sp::Physics>())
                radius = physics->getSize().x;
            glm::vec3 screen_position = worldToScreen(renderer, glm::vec3(transform.getPosition().x, transform.getPosition().y, radius));
            if (screen_position.z < 0.0f)
                continue;
            if (screen_position.z > 10000.0f)
                continue;
            float distance_factor = 1.0f - (screen_position.z / 10000.0f);
            renderer.drawText(sp::Rect(screen_position.x, screen_position.y, 0, 0), callsign.callsign, sp::Alignment::Center, 20 * distance_factor, bold_font, glm::u8vec4(255, 255, 255, 128 * distance_factor));
        }
    }

    if (show_headings && my_spaceship)
    {
        float distance = 2500.f;
        auto transform = my_spaceship.getComponent<sp::Transform>();

        if (transform) {
            for(int angle = 0; angle < 360; angle += 30)
            {
                glm::vec2 world_pos = transform->getPosition() + vec2FromAngle(angle - 90.f) * distance;
                glm::vec3 screen_pos = worldToScreen(renderer, glm::vec3(world_pos.x, world_pos.y, 0.0f));
                if (screen_pos.z > 0.0f)
                    renderer.drawText(sp::Rect(screen_pos.x, screen_pos.y, 0, 0), string(angle), sp::Alignment::Center, 30, bold_font, glm::u8vec4(255, 255, 255, 128));
            }
        }
    }

    glViewport(0, 0, renderer.getPhysicalSize().x, renderer.getPhysicalSize().y);
}

glm::vec3 GuiViewport3D::worldToScreen(sp::RenderTarget& renderer, glm::vec3 world)
{
    auto view_pos = view_matrix * glm::vec4(world, 1.f);
    auto pos = projection_matrix * view_pos;

    // Perspective division
    pos /= pos.w;

    //Window coordinates
    //Map x, y to range 0-1
    glm::vec3 ret;
    ret.x = pos.x * .5f + .5f;
    ret.y = pos.y * .5f + .5f;
    //This is only correct when glDepthRange(0.0, 1.0)
    //ret.z = (1.0+fTempo[6])*0.5;  //Between 0 and 1
    //Set Z to distance into the screen (negative is behind the screen)
    ret.z = -view_pos.z;

    ret.x = rect.position.x + rect.size.x * ret.x;
    ret.y = rect.position.y + rect.size.y * (1.0f - ret.y);
    return ret;
}
