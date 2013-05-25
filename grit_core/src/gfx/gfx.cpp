/* Copyright (c) David Cunningham and the Grit Game Engine project 2013
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <sstream>

#include <OgreAutoParamDataSource.h>
#include <OgreOverlaySystem.h>

#include "../path_util.h"
#include "../sleep.h"
#include "../main.h"

#include "gfx_internal.h"
#include "GfxPipeline.h"
#include "gfx_option.h"
#include "Clutter.h"
#include "GfxMaterial.h"
#include "GfxBody.h"
#include "GfxSkyMaterial.h"
#include "GfxSkyBody.h"
#include "gfx_hud.h"

#ifdef WIN32
bool d3d9 = getenv("GRIT_GL")==NULL;
#else
bool d3d9 = false;
#endif

extern "C" { 
#ifdef WIN32
    _declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
#else
    unsigned int NvOptimusEnablement = 0x00000001;
#endif
}

Ogre::OctreePlugin *octree;
Ogre::CgPlugin *cg;

Ogre::Root *ogre_root = NULL;
Ogre::OverlaySystem *ogre_overlay_system = NULL;
Ogre::RenderSystem *ogre_rs = NULL;
Ogre::RenderWindow *ogre_win = NULL;
Ogre::OctreeSceneManager *ogre_sm = NULL;
Ogre::SceneNode *ogre_root_node = NULL;
Ogre::SharedPtr<Ogre::FrameTimeControllerValue> ftcv;

Ogre::Light *ogre_sun = NULL;

GfxCallback *gfx_cb = NULL;
bool shutting_down = false;
bool use_hwgamma = false; //getenv("GRIT_NOHWGAMMA")==NULL;

static bool reset_frame_buffer_on_next_render = false;

// For default parameters of functions that take GfxStringMap
const GfxStringMap gfx_empty_string_map;

Ogre::Matrix4 shadow_view_proj[3];

Vector3 particle_ambient;
Vector3 fog_colour;
float fog_density;

Vector3 sun_direction;
Vector3 sun_colour;
float sun_alpha;
float sun_size;
float sun_falloff_distance;
float sky_glare_sun_distance;
float sky_glare_horizon_elevation;
float sky_divider[4];
Vector3 sky_colour[6];
Vector3 sky_sun_colour[5];
float sky_alpha[6];
float sky_sun_alpha[5];
Vector3 sky_cloud_colour;
float sky_cloud_coverage;
Vector3 hell_colour;

GfxEnvCubeDiskResource *scene_env_cube = NULL;
float global_exposure = 1;
float global_saturation = 1;
GfxColourGradeLUTDiskResource *scene_colour_grade_lut = NULL;

// abuse ogre fog params to store several things
static void set_ogre_fog (void)
{
    ogre_sm->setFog(Ogre::FOG_EXP2, to_ogre_cv(fog_colour), fog_density, 1, 0);
}


GfxShaderDB shader_db;
GfxMaterialDB material_db;
fast_erase_vector<GfxBody*> gfx_all_bodies;


// {{{ utilities

int freshname_counter = 0;
std::string freshname (const std::string &prefix)
{
    std::stringstream ss;
    ss << prefix << freshname_counter;
    freshname_counter++;
    return ss.str();
}
std::string freshname (void)
{
    return freshname("F:");
}

// }}}


Ogre::Viewport *eye_left_vp = NULL;
Ogre::Viewport *eye_right_vp = NULL;
Ogre::Viewport *hud_vp = NULL;
GfxPipeline *eye_left = NULL;
GfxPipeline *eye_right = NULL;

void do_reset_framebuffer (void)
{
    // get rid of everything that might be set up already

    if (eye_left_vp) ogre_win->removeViewport(eye_left_vp->getZOrder());
    delete eye_left;
    eye_left = NULL;

    if (eye_right_vp) ogre_win->removeViewport(eye_right_vp->getZOrder());
    delete eye_right;
    eye_right = NULL;

    if (hud_vp) ogre_win->removeViewport(hud_vp->getZOrder());

    // set things up again
    if (stereoscopic()) {
        if (gfx_option(GFX_CROSS_EYE)) {
            eye_left_vp = ogre_win->addViewport(NULL, 0, 0, 0, 0.5, 1);
            eye_left = new GfxPipeline("EyeLeft", eye_left_vp);
            eye_right_vp = ogre_win->addViewport(NULL, 1, 0.5, 0, 0.5, 1);
            eye_right = new GfxPipeline("EyeRight", eye_right_vp);
        } else {
            eye_left_vp = ogre_win->addViewport(NULL, 0, 0, 0, 1, 1);
            eye_left = new GfxPipeline("EyeLeft", eye_left_vp);
            eye_right_vp = ogre_win->addViewport(NULL, 1, 0, 0, 1, 1);
            eye_right = new GfxPipeline("EyeRight", eye_right_vp);
        }
    } else {
        eye_left_vp = ogre_win->addViewport(NULL, 0, 0, 0, 1, 1);
        eye_left = new GfxPipeline("EyeLeft", eye_left_vp);
    }

    hud_vp = ogre_win->addViewport(NULL, 10, 0, 0, 1, 1);
}



// {{{ SCENE PROPERTIES

// lighting parameters

Vector3 gfx_sunlight_diffuse (void)
{
    return from_ogre_cv(ogre_sun->getDiffuseColour());;
}

void gfx_sunlight_diffuse (const Vector3 &v)
{
    ogre_sun->setDiffuseColour(to_ogre_cv(v));
}

Vector3 gfx_sunlight_specular (void)
{
    return from_ogre_cv(ogre_sun->getSpecularColour());;
}

void gfx_sunlight_specular (const Vector3 &v)
{
    ogre_sun->setSpecularColour(to_ogre_cv(v));
}

Vector3 gfx_sunlight_direction (void)
{
    return from_ogre(ogre_sun->getDirection());
}

void gfx_sunlight_direction (const Vector3 &v)
{
    ogre_sun->setDirection(to_ogre(v));
}

Vector3 gfx_particle_ambient (void)
{
    return particle_ambient;
}

void gfx_particle_ambient (const Vector3 &v)
{
    particle_ambient = v;
}

GfxEnvCubeDiskResource *gfx_env_cube (void)
{
    return scene_env_cube;
}

void gfx_env_cube (GfxEnvCubeDiskResource *v)
{
    if (v == scene_env_cube) return;
    //CVERB << "Setting scene env cube to " << v << std::endl;

    if (v != NULL) {
        // have to try loading the new one first, in case it fails to load
        v->increment();
        try {
            if (!v->isLoaded()) v->load();
        } catch (GritException &e) {
            v->decrement();
            throw e;
        }
    }

    if (scene_env_cube != NULL) {
        scene_env_cube->decrement();
        bgl->finishedWith(scene_env_cube);
    }

    scene_env_cube = v;
}

GfxColourGradeLUTDiskResource *gfx_colour_grade (void)
{
    return scene_colour_grade_lut;
}

void gfx_colour_grade (GfxColourGradeLUTDiskResource *v)
{
    if (v == scene_colour_grade_lut) return;
    //CVERB << "Setting colour grade to " << v << std::endl;

    if (v != NULL) {
        // have to try loading the new one first, in case it fails to load
        v->increment();
        try {
            if (!v->isLoaded()) v->load();
        } catch (GritException &e) {
            v->decrement();
            throw e;
        }
    }

    if (scene_colour_grade_lut != NULL) {
        scene_colour_grade_lut->decrement();
        bgl->finishedWith(scene_colour_grade_lut);
    }

    scene_colour_grade_lut = v;
}

float gfx_global_saturation (void)
{
    return global_saturation;
}

void gfx_global_saturation (float v)
{
    global_saturation = v;
}
    
float gfx_global_exposure (void)
{
    return global_exposure;
}

void gfx_global_exposure (float v)
{
    global_exposure = v;
}
    


Vector3 gfx_fog_colour (void)
{
    return fog_colour;
}

void gfx_fog_colour (const Vector3 &v)
{
    fog_colour = v;
    set_ogre_fog();
}

float gfx_fog_density (void)
{
    return fog_density;
}

void gfx_fog_density (float v)
{
    fog_density = v;
    set_ogre_fog();
}


float gfx_sun_size (void)
{
    return sun_size;
}

void gfx_sun_size (float v)
{
    sun_size = v;
}

float gfx_sun_falloff_distance (void)
{
    return sun_falloff_distance;
}

void gfx_sun_falloff_distance (float v)
{
    sun_falloff_distance = v;
}

Vector3 gfx_sky_cloud_colour (void)
{
    return sky_cloud_colour;
}

void gfx_sky_cloud_colour (const Vector3 &v)
{
    sky_cloud_colour = v;
}

float gfx_sky_cloud_coverage (void)
{
    return sky_cloud_coverage;
}

void gfx_sky_cloud_coverage (float v)
{
    sky_cloud_coverage = v;
}

Vector3 gfx_hell_colour (void)
{
    return hell_colour;
}

void gfx_hell_colour (const Vector3 &v)
{
    hell_colour = v;
}

Vector3 gfx_sun_direction (void)
{
    return sun_direction;
}

void gfx_sun_direction (const Vector3 &v)
{
    sun_direction = v;
}

Vector3 gfx_sun_colour (void)
{
    return sun_colour;
}

void gfx_sun_colour (const Vector3 &v)
{
    sun_colour = v;
}

float gfx_sun_alpha (void)
{
    return sun_alpha;
}

void gfx_sun_alpha (float v)
{
    sun_alpha = v;
}

float gfx_sky_glare_sun_distance (void)
{
    return sky_glare_sun_distance;
}

void gfx_sky_glare_sun_distance (float v)
{
    sky_glare_sun_distance = v;
}

float gfx_sky_glare_horizon_elevation (void)
{
    return sky_glare_horizon_elevation;
}

void gfx_sky_glare_horizon_elevation (float v)
{
    sky_glare_horizon_elevation = v;
}

float gfx_sky_divider (unsigned i)
{
    APP_ASSERT(i < 4);
    return sky_divider[i];
}

void gfx_sky_divider (unsigned i, float v)
{
    APP_ASSERT(i < 4);
    sky_divider[i] = v;
}

Vector3 gfx_sky_colour (unsigned i)
{
    APP_ASSERT(i < 6);
    return sky_colour[i];
}

void gfx_sky_colour (unsigned i, const Vector3 &v)
{
    APP_ASSERT(i < 6);
    sky_colour[i] = v;
}

Vector3 gfx_sky_sun_colour (unsigned i)
{
    APP_ASSERT(i < 5);
    return sky_sun_colour[i];
}

void gfx_sky_sun_colour (unsigned i, const Vector3 &v)
{
    APP_ASSERT(i < 5);
    sky_sun_colour[i] = v;
}

float gfx_sky_alpha (unsigned i)
{
    APP_ASSERT(i < 6);
    return sky_alpha[i];
}

void gfx_sky_alpha (unsigned i, float v)
{
    APP_ASSERT(i < 6);
    sky_alpha[i] = v;
}

float gfx_sky_sun_alpha (unsigned i)
{
    APP_ASSERT(i < 5);
    return sky_sun_alpha[i];
}

void gfx_sky_sun_alpha (unsigned i, float v)
{
    APP_ASSERT(i < 5);
    sky_sun_alpha[i] = v;
}

// }}}


// {{{ RENDER

void update_coronas (const Vector3 &cam_pos);

float anim_time = 0;

static float time_since_started_rendering = 0;

void gfx_render (float elapsed, const Vector3 &cam_pos, const Quaternion &cam_dir)
{
    time_since_started_rendering += elapsed;
    anim_time = fmodf(anim_time+elapsed, ANIM_TIME_MAX);

    debug_drawer->frameStarted();

    try {
        Ogre::WindowEventUtilities::messagePump();
		if (reset_frame_buffer_on_next_render) {
            reset_frame_buffer_on_next_render = false;
            do_reset_framebuffer();
        }


        update_coronas(cam_pos);

        handle_dirty_materials();

        // This pumps ogre's texture animation and probably other things
        ftcv->setValue(elapsed);
        ftcv->setElapsedTime(time_since_started_rendering);
        // used for indicating that ogre internal data prepared for last frame is now invalid
        ogre_root->setNextFrameNumber(ogre_root->getNextFrameNumber()+1);

        if (ogre_win->isActive()) {
            ogre_win->_beginUpdate();
            CameraOpts opts_left;
            opts_left.fovY = gfx_option(GFX_FOV);
            opts_left.nearClip = gfx_option(GFX_NEAR_CLIP);
            opts_left.farClip = gfx_option(GFX_FAR_CLIP);
            opts_left.pos = cam_pos;
            opts_left.dir = cam_dir;
            opts_left.bloomAndToneMap = gfx_option(GFX_POST_PROCESSING);
            opts_left.particles = gfx_option(GFX_RENDER_PARTICLES);
            opts_left.pointLights = gfx_option(GFX_POINT_LIGHTS);
            opts_left.sky = gfx_option(GFX_RENDER_SKY);
            if (stereoscopic()) {

                float FOV = gfx_option(GFX_FOV);
                float monitor_height = gfx_option(GFX_MONITOR_HEIGHT);
                float distance = gfx_option(GFX_MONITOR_EYE_DISTANCE);
                float eye_separation = gfx_option(GFX_EYE_SEPARATION);
                float min = gfx_option(GFX_MIN_PERCEIVED_DEPTH);
                float max = gfx_option(GFX_MAX_PERCEIVED_DEPTH);

                CameraOpts opts_right = opts_left;

                float s = 2*tan((FOV/2)/180*M_PI)/monitor_height * (eye_separation * (1-distance/max));
                opts_left.frustumOffset = s/2;
                opts_right.frustumOffset = -s/2;
        
                // cam separation -- different than eye separation because we want to control the perceived depth
                float c = 2*tan((FOV/2)/180*M_PI)/monitor_height * (eye_separation * (1-distance/min));
                c = gfx_option(GFX_NEAR_CLIP) * (s - c);

                opts_left.pos = cam_pos + c/2 * (cam_dir*Vector3(-1,0,0));
                opts_right.pos = cam_pos + c/2 * (cam_dir*Vector3(1,0,0));

                if (gfx_option(GFX_ANAGLYPH)) {
                    opts_left.mask = Vector3(gfx_option(GFX_ANAGLYPH_LEFT_RED_MASK),
                                             gfx_option(GFX_ANAGLYPH_LEFT_GREEN_MASK),
                                             gfx_option(GFX_ANAGLYPH_LEFT_BLUE_MASK));
                    opts_right.mask = Vector3(gfx_option(GFX_ANAGLYPH_RIGHT_RED_MASK),
                                              gfx_option(GFX_ANAGLYPH_RIGHT_GREEN_MASK),
                                              gfx_option(GFX_ANAGLYPH_RIGHT_BLUE_MASK));
                    opts_left.saturationMask = opts_right.saturationMask = 1 - gfx_option(GFX_ANAGLYPH_DESATURATION);

                }

                eye_left->render(opts_left);
                eye_right->render(opts_right, gfx_option(GFX_ANAGLYPH));

            } else {
                eye_left->render(opts_left);
            }

            gfx_hud_render(hud_vp);

            ogre_win->_endUpdate();

            ogre_rs->_swapAllRenderTargetBuffers(true);
        } else {
            // corresponds to 100fps
            mysleep(10000);
        }


    } catch (Ogre::Exception &e) {
        CERR << e.getFullDescription() << std::endl;
    } catch (GritException &e) {
        CERR << e.msg << std::endl;
    }

    debug_drawer->frameEnded();
}

// }}}

void gfx_bake_env_cube (const std::string &filename, unsigned size, const Vector3 &cam_pos, float saturation, const Vector3 &ambient)
{
    if ((size & (size-1)) != 0) GRIT_EXCEPT("Can only bake env cubes with power of 2 size");

    // make texture
    Ogre::TexturePtr cube = Ogre::TextureManager::getSingleton().createManual("env_cube_bake", RESGRP, Ogre::TEX_TYPE_2D,
                    6 * size, size, 1,
                    0,
                    Ogre::PF_FLOAT32_RGB,
                    Ogre::TU_RENDERTARGET,
                    NULL,
                    false);
    Ogre::RenderTarget *rt = cube->getBuffer()->getRenderTarget();

    // these are correct assuming the end image is going to be flipped about y
    Quaternion cube_orientations[6] = {
        Quaternion(Degree(90), Vector3(0,1,0)) * Quaternion(Degree(90), Vector3(1,0,0)), // lean your head back to look up, then lean to the right
        Quaternion(Degree(90), Vector3(0,-1,0)) * Quaternion(Degree(90), Vector3(1,0,0)), // lean your head back to look up, then lean to the left
        Quaternion(1,0,0,0), // looking north
        Quaternion(Degree(180), Vector3(1,0,0)), // lean your head back until you are looking south
        Quaternion(Degree(90), Vector3(1,0,0)), // lean your head back to look up
        Quaternion(Degree(90), Vector3(1,0,0)) * Quaternion(Degree(180), Vector3(0,0,1)), // turn to face south, then look down
    };

    // create 6 viewports and 6 pipelines
    for (unsigned i=0 ; i<6 ; ++i) {
        Ogre::Viewport *vp = rt->addViewport(NULL, 0, i/6.0, 0, 1.0/6, 1);

        GfxPipeline pipe("env_cube_bake_pipe", vp);

        CameraOpts opts;
        opts.fovY = 90;
        opts.nearClip = 0.3;
        opts.farClip = 1200;
        opts.pos = cam_pos;
        opts.dir = cube_orientations[i];
        opts.bloomAndToneMap = false;
        opts.particles = false;
        opts.pointLights = false;
        opts.sky = true;
        opts.hud = false;
        opts.sun = false;

        pipe.render(opts);

        rt->removeViewport(vp->getZOrder());
    }

    // read back onto cpu
    Ogre::Image img_raw;
    unsigned char *img_raw_buf = OGRE_ALLOC_T(unsigned char, 6*size*size*3*4, Ogre::MEMCATEGORY_GENERAL);
    img_raw.loadDynamicImage(img_raw_buf, size*6, size, 1, Ogre::PF_FLOAT32_RGB, true);
    rt->copyContentsToMemory(img_raw.getPixelBox());
    Ogre::PixelBox img_raw_box = img_raw.getPixelBox();

    // clean up texture
    Ogre::TextureManager::getSingleton().remove(cube->getName());
    rt = NULL;

    // make an image that is a target for the conversion process
    Ogre::Image img_conv;
    unsigned char *img_conv_buf = OGRE_ALLOC_T(unsigned char, 6*size*size*3*2, Ogre::MEMCATEGORY_GENERAL);
    img_conv.loadDynamicImage(img_conv_buf, size*6, size, 1, Ogre::PF_SHORT_RGB, true);
    Ogre::PixelBox img_conv_box = img_conv.getPixelBox();

    // do conversion (flip y, gamma, hdr range up to 16)
    for (unsigned y=0 ; y<size ; y++) {
        for (unsigned x=0 ; x<size*6 ; x++) {
            Ogre::ColourValue cv = img_raw.getColourAt(x,y,0);
            cv = cv + Ogre::ColourValue(ambient.x, ambient.y, ambient.z);
            float grey = (cv.r + cv.g + cv.b)/3;
            cv = saturation * cv + (1-saturation) * Ogre::ColourValue(grey, grey, grey, 1);
            cv = cv / 16;
            cv = Ogre::ColourValue(powf(cv.r,1/2.2), powf(cv.g,1/2.2), powf(cv.b,1/2.2), 1.0f);
            img_conv.setColourAt(cv,x,size - y - 1,0);
        }
    }

    // write out to file
    img_conv.save(filename);

}


void gfx_screenshot (const std::string &filename) { ogre_win->writeContentsToFile(filename); }

static GfxLastRenderStats stats_from_rt (Ogre::RenderTarget *rt)
{
    GfxLastRenderStats r;
    r.batches = float(rt->getBatchCount());
    r.triangles = float(rt->getTriangleCount());
    return r;
}

GfxLastFrameStats gfx_last_frame_stats (void)
{
    GfxLastFrameStats r; 

    r.left_deferred = eye_left->getDeferredStats();
    r.left_gbuffer = eye_left->getGBufferStats();

    if (stereoscopic()) {
        r.right_deferred = eye_right->getGBufferStats();
        r.right_gbuffer = eye_right->getGBufferStats();
    }

    if (gfx_option(GFX_SHADOW_CAST)) {
        for (int i=0 ; i<3 ; ++i) {
            r.shadow[i] = stats_from_rt(ogre_sm->getShadowTexture(i)->getBuffer()->getRenderTarget());
        }
    }

    return r;
}

GfxRunningFrameStats gfx_running_frame_stats (void)
{
    GfxRunningFrameStats r;
    return r;
}


// {{{ LISTENERS 

struct MeshSerializerListener : Ogre::MeshSerializerListener {
    void processMaterialName (Ogre::Mesh *mesh, std::string *name)
    {
        if (shutting_down) return;
        std::string filename = mesh->getName();
        std::string dir(filename, 0, filename.rfind('/')+1);
        *name = pwd_full_ex(*name, "/"+dir, "BaseWhite");
    }

    void processSkeletonName (Ogre::Mesh *mesh, std::string *name)
    {
        if (shutting_down) return;
        std::string filename = mesh->getName();
        std::string dir(filename, 0, filename.rfind('/')+1);
        *name = pwd_full_ex(*name, "/"+dir, *name).substr(1); // strip leading '/' from this one
    }
    void processMeshCompleted (Ogre::Mesh *mesh)
    {
        (void) mesh;
        // do nothing
    }
} mesh_serializer_listener;

struct WindowEventListener : Ogre::WindowEventListener {

    void windowResized(Ogre::RenderWindow *rw)
    {
        if (shutting_down) return;
        gfx_cb->windowResized(rw->getWidth(),rw->getHeight());
		reset_frame_buffer_on_next_render = true;
        gfx_hud_signal_window_resized(rw->getWidth(),rw->getHeight());
    }

    void windowClosed (Ogre::RenderWindow *rw)
    {
        (void) rw;
        if (shutting_down) return;
        gfx_cb->clickedClose();
    }

} window_event_listener;

struct LogListener : Ogre::LogListener {
    virtual void messageLogged (const std::string &message,
                                Ogre::LogMessageLevel lml,
                                bool maskDebug,
                                const std::string &logName,
                                bool& skipThisMessage )

    {
        (void)lml;
        (void)logName;
        (void)skipThisMessage;
        if (!maskDebug) gfx_cb->messageLogged(message);
    }
} log_listener;

struct SceneManagerListener : Ogre::SceneManager::Listener {
    //virtual void preUpdateSceneGraph (Ogre::SceneManager *, Ogre::Camera *camera)
    //{
    //    //CVERB << "preUpdateSceneGraph: " << camera->getName() << std::endl;
    //}

    //virtual void preFindVisibleObjects (Ogre::SceneManager *, Ogre::SceneManager::IlluminationRenderStage irs, Ogre::Viewport *v)
    //{
    //    //CVERB << "preFindVisibleObjects: " << irs << " " << v << std::endl;
    //}

    //virtual void postFindVisibleObjects (Ogre::SceneManager *, Ogre::SceneManager::IlluminationRenderStage irs, Ogre::Viewport *v)
    //{
    //    //CVERB << "postFindVisibleObjects: " << irs << " " << v << std::endl;
    //}

    virtual void shadowTexturesUpdated (size_t numberOfShadowTextures)
    {
        (void) numberOfShadowTextures;
        // misleading, actually refers to number of lights that have had shadow textures populated i think
        //APP_ASSERT(numberOfShadowTextures==1);
        //if (numberOfShadowTextures!=1)
        //    CVERB << "shadowTexturesUpdated: " << numberOfShadowTextures << std::endl;
    }

    virtual void shadowTextureCasterPreViewProj (Ogre::Light *light, Ogre::Camera *cam, size_t iteration)
    {
        // apparently other lights cast shadows, should probably fix that...
        if (light != ogre_sun) return;
        APP_ASSERT(iteration < 3);
        //CVERB << "shadowTextureCasterPreViewProj: " << light->getName() << " " << cam->getName() <<  " " << iteration << std::endl;
        Ogre::Matrix4 view = cam->getViewMatrix();
        Ogre::Matrix4 proj = cam->getProjectionMatrixWithRSDepth();
        static const Ogre::Matrix4 to_uv_space( 0.5,    0,    0,  0.5, 0,   -0.5,    0,  0.5, 0,      0,    1,    0, 0,      0,    0,    1);
        Ogre::Matrix4 view_proj = to_uv_space*proj*view;
        shadow_view_proj[iteration] = view_proj;
        //CVERB << light->getName() << " " << iteration << " " << cam->getViewMatrix() << std::endl;
        //CVERB << light->getName() << " " << iteration << " " << cam->getProjectionMatrixRS() << std::endl;
    }

    virtual void shadowTextureReceiverPreViewProj (Ogre::Light *light, Ogre::Frustum *frustum)
    {
        CVERB << "shadowTextureReceiverPreViewProj: " << light->getName() << " " << frustum->getName() << std::endl;
    }

    //virtual bool sortLightsAffectingFrustum (Ogre::LightList &lightList)
    //{
    //    //CVERB << "sortLightsAffectingFrustum: " << lightList.size() << std::endl;
    //    return false;
    //}

    //virtual void sceneManagerDestroyed (Ogre::SceneManager *)
    //{
    //    //CVERB << "sceneManagerDestroyed" << std::endl;
    //}

} ogre_sm_listener;

// }}}


// {{{ INIT / SHUTDOWN

size_t gfx_init (GfxCallback &cb_)
{
    try {
        gfx_cb = &cb_;

        Ogre::LogManager *lmgr = OGRE_NEW Ogre::LogManager();
        Ogre::Log *ogre_log = OGRE_NEW Ogre::Log("",false,true);
        ogre_log->addListener(&log_listener);
        lmgr->setDefaultLog(ogre_log);
        lmgr->setLogDetail(Ogre::LL_NORMAL);

        ogre_root = OGRE_NEW Ogre::Root("","","");

        octree = OGRE_NEW Ogre::OctreePlugin();
        ogre_root->installPlugin(octree);

        cg = OGRE_NEW Ogre::CgPlugin();
        ogre_root->installPlugin(cg);

        if (d3d9) {
            #ifdef WIN32
            ogre_rs = OGRE_NEW Ogre::D3D9RenderSystem(GetModuleHandle(NULL));
            ogre_rs->setConfigOption("Allow NVPerfHUD","Yes");
            ogre_rs->setConfigOption("Floating-point mode","Consistent");
            ogre_rs->setConfigOption("Video Mode","1024 x 768 @ 32-bit colour");
            #endif
        } else {
            ogre_rs = OGRE_NEW Ogre::GLRenderSystem();
            ogre_rs->setConfigOption("RTT Preferred Mode","FBO");
            ogre_rs->setConfigOption("Video Mode","1024 x 768");
        }
        ogre_rs->setConfigOption("sRGB Gamma Conversion",use_hwgamma?"Yes":"No");
        ogre_rs->setConfigOption("Full Screen","No");
        ogre_rs->setConfigOption("VSync","Yes");

        Ogre::ConfigOptionMap &config_opts = ogre_rs->getConfigOptions();
        CLOG << "Rendersystem options:" << std::endl;
        for (Ogre::ConfigOptionMap::iterator i=config_opts.begin(),i_=config_opts.end() ; i!=i_ ; ++i) {
            const Ogre::StringVector &sv = i->second.possibleValues;
            CLOG << "    " << i->second.name << " (" << (i->second.immutable ? "immutable" : "mutable") << ")  {";
            for (unsigned j=0 ; j<sv.size() ; ++j) {
                CLOG << (j==0?" ":", ") << sv[j];
            }
            CLOG << " }" << std::endl;
        }
        ogre_root->setRenderSystem(ogre_rs);

        ogre_overlay_system = new Ogre::OverlaySystem();

        ogre_root->initialise(true,"Grit Game Window");

        ogre_win = ogre_root->getAutoCreatedWindow();

        size_t winid;
        ogre_win->getCustomAttribute("WINDOW", &winid);
        #ifdef WIN32
        HMODULE mod = GetModuleHandle(NULL);
        HICON icon_big = (HICON)LoadImage(mod, MAKEINTRESOURCE(118), IMAGE_ICON,
                                          0, 0, LR_DEFAULTSIZE|LR_SHARED);
        HICON icon_small = (HICON)LoadImage(mod,MAKEINTRESOURCE(118), IMAGE_ICON,
                                          16, 16, LR_DEFAULTSIZE|LR_SHARED);
        SendMessage((HWND)winid, (UINT)WM_SETICON, (WPARAM) ICON_BIG, (LPARAM) icon_big);
        SendMessage((HWND)winid, (UINT)WM_SETICON, (WPARAM) ICON_SMALL, (LPARAM) icon_small);
        #endif

        ogre_win->setDeactivateOnFocusChange(false);

        Ogre::TextureManager::getSingleton().setVerbose(false);
        Ogre::MeshManager::getSingleton().setVerbose(false);
        Ogre::OverlayManager::getSingleton()
                .addOverlayElementFactory(new HUD::TextListOverlayElementFactory());
        ogre_root->addMovableObjectFactory(new MovableClutterFactory());
        ogre_root->addMovableObjectFactory(new RangedClutterFactory());

        Ogre::MeshManager::getSingleton().setListener(&mesh_serializer_listener);
        Ogre::WindowEventUtilities::addWindowEventListener(ogre_win, &window_event_listener);
        Ogre::ResourceGroupManager::getSingleton().addResourceLocation(".","FileSystem",RESGRP,false);
        Ogre::ResourceGroupManager::getSingleton().initialiseAllResourceGroups();
        
        ftcv = Ogre::ControllerManager::getSingleton().getFrameTimeSource().dynamicCast<Ogre::FrameTimeControllerValue>();
        if (ftcv.isNull()) {
            CERR << "While initialising Grit, Ogre::FrameControllerValue could not be found!" << std::endl;
        }
        ogre_sm = static_cast<Ogre::OctreeSceneManager*>(ogre_root->createSceneManager("OctreeSceneManager"));
        ogre_sm->addListener(&ogre_sm_listener);
        ogre_root_node = ogre_sm->getRootSceneNode();
        ogre_sm->setShadowCasterRenderBackFaces(false);
        ogre_sm->setShadowTextureSelfShadow(true);
        ogre_sm->addRenderQueueListener(ogre_overlay_system);
        ogre_sun = ogre_sm->createLight("Sun");
        ogre_sun->setType(Ogre::Light::LT_DIRECTIONAL);

        gfx_pipeline_init();
        gfx_option_init();
        gfx_particle_init();
        gfx_hud_init();
 
        gfx_env_cube(NULL);

        return winid;
    } catch (Ogre::Exception &e) {
        GRIT_EXCEPT2(e.getFullDescription(), "Initialising graphics subsystem");
    }
}

HUD::RootPtr gfx_init_hud (void)
{
    return HUD::RootPtr(new HUD::Root(ogre_win->getWidth(),ogre_win->getHeight()));
}

GfxMaterialType gfx_material_type (const std::string &name)
{
    GFX_MAT_SYNC;
    if (!gfx_material_has_any(name)) GRIT_EXCEPT("Non-existent material: \""+name+"\"");
    GfxBaseMaterial *mat = material_db[name];
    if (dynamic_cast<GfxMaterial*>(mat) != NULL) return GFX_MATERIAL;
    if (dynamic_cast<GfxSkyMaterial*>(mat) != NULL) return GFX_SKY_MATERIAL;
    GRIT_EXCEPT("Internal error: unrecognised kind of material");
    return GFX_MATERIAL; // never get here
}

bool gfx_material_has_any (const std::string &name)
{
    GFX_MAT_SYNC;
    return material_db.find(name) != material_db.end();
}

void gfx_material_add_dependencies (const std::string &name, DiskResource *into)
{
    if (!gfx_material_has_any(name)) GRIT_EXCEPT("Non-existent material: \""+name+"\"");
    material_db[name]->addDependencies(into);
}

void gfx_shutdown (void)
{
    try {
        if (shutting_down) return;
        shutting_down = true;
        delete eye_left;
        delete eye_right;
        ftcv.setNull();
        gfx_hud_shutdown();
        if (ogre_sm && ogre_root) ogre_root->destroySceneManager(ogre_sm);
        if (ogre_overlay_system && ogre_root) OGRE_DELETE ogre_overlay_system;
        if (ogre_root) OGRE_DELETE ogre_root; // internally deletes ogre_rs
        OGRE_DELETE octree;
        OGRE_DELETE cg;
    } catch (Ogre::Exception &e) {
        GRIT_EXCEPT2(e.getFullDescription(), "Shutting down graphics subsystem");
    }

}

// }}}



// {{{ COLOURSPACE CONVERSION UTILS

void RGBtoHSL (float R, float G, float B, float &H, float &S, float &L)
{

    float max_intensity = std::max(std::max(R,G),B);
    float min_intensity = std::min(std::min(R,G),B);

    float delta = max_intensity - min_intensity;

    L = 0.5f * (max_intensity + min_intensity); 

    if (delta == 0) {
        // all channels the same (colour is grey)
        S = 0.0f;
        H = 0.0f;
        return;
    }

    if (L < 0.5f) {
        S = (max_intensity - min_intensity)/(max_intensity + min_intensity);
    } else {
        S = (max_intensity - min_intensity)/(2 - max_intensity - min_intensity);
    }
    if (max_intensity == R) {
        H = (G - B)/delta;
    }
    if (max_intensity == G) {
        H = 2 + (B - R)/delta;
    }
    if (max_intensity == B) {
        H = 4 + (R - G)/delta; 
    }
    H /= 6;
    if (H < 0) H += 1;
}
    
static float HSLtoRGB_aux (float temp1, float temp2, float temp3)
{   
    if (temp3 < 1)      return temp2  +  (temp1-temp2) * temp3; 
    else if (temp3 < 3) return temp1;
    else if (temp3 < 4) return temp2  +  (temp1-temp2) * (4 - temp3);
    else                return temp2;
}   

void HSLtoRGB (float H, float S, float L, float &R, float &G, float &B)
{   
    if (S == 0) { 
        // grey
        R = L;
        G = L;
        B = L;
        return;
    }

    float temp1 = L<0.5f ? L + L*S : L + S - L*S;
    float temp2 = 2*L - temp1;

    R = HSLtoRGB_aux(temp1, temp2, fmodf(6*H + 2, 6));
    G = HSLtoRGB_aux(temp1, temp2, 6*H);
    B = HSLtoRGB_aux(temp1, temp2, fmodf(6*H + 4, 6));
}


void HSVtoHSL (float h, float s, float v, float &hh, float &ss, float &ll)
{
    hh = h;
    ll = (2 - s) * v;
    ss = s * v;
    ss /= (ll <= 1) ? ll : 2 - ll;
    ll /= 2;
}

void HSLtoHSV (float hh, float ss, float ll, float &h, float &s, float &v)
{
    h = hh;
    ss *= (ll <= 0.5) ? ll : 1 - ll;
    v = ll + ss;
    s = 2 * ss / (ll + ss);
}

void RGBtoHSV (float R, float G, float B, float &H, float &S, float &V)
{
    float max_intensity = std::max(std::max(R,G),B);
    float min_intensity = std::min(std::min(R,G),B);

    V = max_intensity;

    float delta = max_intensity - min_intensity;

    if (delta == 0) {
        // grey
        H = 0;
        S = 0;
        return;
    }

    S = (delta / max_intensity);

    if (max_intensity == R) {
        H = (G - B)/delta;
    }
    if (max_intensity == G) {
        H = 2 + (B - R)/delta;
    }
    if (max_intensity == B) {
        H = 4 + (R - G)/delta;
    }
    H /= 6;
    if (H < 0) H += 1;
}


void HSVtoRGB (float H, float S, float V, float &R, float &G, float &B)
{
    if (S == 0.0) {
        // grey
        R = V;
        G = V;
        B = V;
        return;
    }

    float hh = fmodf(H * 6, 6);
    long i = (long)hh;
    float ff = hh - i;
    float p = V * (1.0 - S);
    float q = V * (1.0 - (S * ff));
    float t = V * (1.0 - (S * (1.0 - ff)));

    switch (i) {

        case 0:
        R = V;
        G = t;
        B = p;
        break;

        case 1:
        R = q;
        G = V;
        B = p;
        break;

        case 2:
        R = p;
        G = V;
        B = t;
        break;

        case 3:
        R = p;
        G = q;
        B = V;
        break;

        case 4:
        R = t;
        G = p;
        B = V;
        break;

        case 5:
        default:
        R = V;
        G = p;
        B = q;
        break;
    }
}

Vector3 gfx_colour_grade_look_up (const Vector3 &v)
{
    if (scene_colour_grade_lut == NULL) return v;
    return scene_colour_grade_lut->lookUp(v);
}

// }}}
