#define SDL_MAIN_USE_CALLBACKS 1

#include "texture_utils.h"
#include "webgpu_context.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <cglm/cglm.h>

#if defined(__ANDROID__) || defined(SDL_PLATFORM_ANDROID)
#define ASSET_PATH(path) path
#else
#define ASSET_PATH(path) "assets/" path
#endif

#define NUM_CRATES 3
#define NUM_OBJECTS (NUM_CRATES + 1)
#define UNIFORM_ALIGNMENT 256

typedef struct SpriteTransform
{
    vec2 pos;
    float rotation;
    vec2 scale;
} SpriteTransform;

static SDL_Window *window = NULL;
static TTF_Font *font = NULL;

static WGPURenderPipeline pipeline = NULL;
static WGPURenderPipeline text_pipeline = NULL;

static WGPUBuffer uniform_buffer = NULL;

static WGPUBindGroupLayout bind_group_layout = NULL;
static WGPUBindGroup crate_bind_group = NULL;
static WGPUBindGroup text_bind_group = NULL;

static WebGPUTexture crate_texture = { 0 };
static WebGPUTexture text_texture = { 0 };

static SpriteTransform crate_sprites[NUM_CRATES] = {
    { { -100.0f, 50.0f }, 15.0f, { 80.0f, 80.0f } },
    { { 0.0f, -20.0f }, -30.0f, { 100.0f, 100.0f } },
    { { 110.0f, 60.0f }, 45.0f, { 70.0f, 70.0f } }
};

static SpriteTransform text_sprite = {
    { 0.0f, -120.0f },
    10.0f,
    { 1.0f, 1.0f }
};

struct Uniforms
{
    mat4 mvp;
};

typedef struct Uniforms Uniforms;

#if defined(__ANDROID__) || defined(SDL_PLATFORM_ANDROID)
static const int WIN_WIDTH = 1280;
static const int WIN_HEIGHT = 720;
#else
static const int WIN_WIDTH = 400;
static const int WIN_HEIGHT = 400;
#endif

static const char *shader_code =
    "struct Uniforms {\n"
    "    mvp: mat4x4f,\n"
    "};\n"

    "@group(0) @binding(0) var<uniform> u: Uniforms;\n"
    "@group(0) @binding(1) var t_diffuse: texture_2d<f32>;\n"
    "@group(0) @binding(2) var s_diffuse: sampler;\n\n"

    "struct VertexOutput {\n"
    "    @builtin(position) position: vec4f,\n"
    "    @location(0) uv: vec2f,\n"
    "};\n\n"

    "@vertex\n"
    "fn vs_main(@builtin(vertex_index) in_vertex_index: u32) -> VertexOutput {\n"

    "    var pos = array<vec2f, 6>(\n"
    "        vec2f(-0.5,  0.5),\n"
    "        vec2f(-0.5, -0.5),\n"
    "        vec2f( 0.5, -0.5),\n"
    "        vec2f(-0.5,  0.5),\n"
    "        vec2f( 0.5, -0.5),\n"
    "        vec2f( 0.5,  0.5)\n"
    "    );\n"

    "    var uvs = array<vec2f, 6>(\n"
    "        vec2f(0.0, 0.0),\n"
    "        vec2f(0.0, 1.0),\n"
    "        vec2f(1.0, 1.0),\n"
    "        vec2f(0.0, 0.0),\n"
    "        vec2f(1.0, 1.0),\n"
    "        vec2f(1.0, 0.0)\n"
    "    );\n"

    "    var out: VertexOutput;\n"
    "    out.position = u.mvp * vec4f(pos[in_vertex_index], 0.0, 1.0);\n"
    "    out.uv = uvs[in_vertex_index];\n"
    "    return out;\n"
    "}\n\n"

    "@fragment\n"
    "fn fs_main(in: VertexOutput) -> @location(0) vec4f {\n"
    "    return textureSample(t_diffuse, s_diffuse, in.uv);\n"
    "}\n";

static const char *text_shader_code =
    "struct Uniforms {\n"
    "    mvp: mat4x4f,\n"
    "};\n"

    "@group(0) @binding(0) var<uniform> u: Uniforms;\n"
    "@group(0) @binding(1) var t_diffuse: texture_2d<f32>;\n"
    "@group(0) @binding(2) var s_diffuse: sampler;\n\n"

    "struct VertexOutput {\n"
    "    @builtin(position) position: vec4f,\n"
    "    @location(0) uv: vec2f,\n"
    "};\n\n"

    "@vertex\n"
    "fn vs_main(@builtin(vertex_index) in_vertex_index: u32) -> VertexOutput {\n"

    "    var pos = array<vec2f, 6>(\n"
    "        vec2f(-0.5,  0.5),\n"
    "        vec2f(-0.5, -0.5),\n"
    "        vec2f( 0.5, -0.5),\n"
    "        vec2f(-0.5,  0.5),\n"
    "        vec2f( 0.5, -0.5),\n"
    "        vec2f( 0.5,  0.5)\n"
    "    );\n"

    "    var uvs = array<vec2f, 6>(\n"
    "        vec2f(0.0, 1.0),\n"
    "        vec2f(0.0, 0.0),\n"
    "        vec2f(1.0, 0.0),\n"
    "        vec2f(0.0, 1.0),\n"
    "        vec2f(1.0, 0.0),\n"
    "        vec2f(1.0, 1.0)\n"
    "    );\n"

    "    var out: VertexOutput;\n"
    "    out.position = u.mvp * vec4f(pos[in_vertex_index], 0.0, 1.0);\n"
    "    out.uv = uvs[in_vertex_index];\n"
    "    return out;\n"
    "}\n\n"

    "@fragment\n"
    "fn fs_main(in: VertexOutput) -> @location(0) vec4f {\n"
    "    return textureSample(t_diffuse, s_diffuse, in.uv);\n"
    "}\n";

static WGPURenderPipeline CreatePipeline(const char *source, WGPUPipelineLayout pipeline_layout)
{
    WGPUShaderSourceWGSL wgsl_desc;
    SDL_memset(&wgsl_desc, 0, sizeof(wgsl_desc));
    wgsl_desc.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl_desc.code = WGPU_STR(source);

    WGPUShaderModuleDescriptor shader_desc;
    SDL_memset(&shader_desc, 0, sizeof(shader_desc));
    shader_desc.nextInChain = &wgsl_desc.chain;

    WGPUShaderModule shader_module = wgpuDeviceCreateShaderModule(g_gpu.device, &shader_desc);

    WGPUBlendState blend_state;
    SDL_memset(&blend_state, 0, sizeof(blend_state));
    blend_state.color.operation = WGPUBlendOperation_Add;
    blend_state.color.srcFactor = WGPUBlendFactor_SrcAlpha;
    blend_state.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend_state.alpha.operation = WGPUBlendOperation_Add;
    blend_state.alpha.srcFactor = WGPUBlendFactor_One;
    blend_state.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

    WGPUColorTargetState color_target;
    SDL_memset(&color_target, 0, sizeof(color_target));
    color_target.format = g_gpu.config.format;
    color_target.blend = &blend_state;
    color_target.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragment_state;
    SDL_memset(&fragment_state, 0, sizeof(fragment_state));
    fragment_state.module = shader_module;
    fragment_state.entryPoint = WGPU_STR("fs_main");
    fragment_state.targetCount = 1;
    fragment_state.targets = &color_target;

    WGPURenderPipelineDescriptor pipeline_desc;
    SDL_memset(&pipeline_desc, 0, sizeof(pipeline_desc));
    pipeline_desc.layout = pipeline_layout;
    pipeline_desc.vertex.module = shader_module;
    pipeline_desc.vertex.entryPoint = WGPU_STR("vs_main");
    pipeline_desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipeline_desc.primitive.frontFace = WGPUFrontFace_CCW;
    pipeline_desc.primitive.cullMode = WGPUCullMode_None;
    pipeline_desc.multisample.count = 1;
    pipeline_desc.multisample.mask = 0xFFFFFFFF;
    pipeline_desc.multisample.alphaToCoverageEnabled = false;
    pipeline_desc.fragment = &fragment_state;

    WGPURenderPipeline result = wgpuDeviceCreateRenderPipeline(g_gpu.device, &pipeline_desc);

    wgpuShaderModuleRelease(shader_module);

    return result;
}

static bool InitPipeline(void)
{
    LogApp(">>> Loading font asset and generating text surface...");

    font = TTF_OpenFont(ASSET_PATH("fonts/LiberationSans-Regular.ttf"), 36.0f);

    if (!font)
    {
        SDL_Log("Font error: %s", SDL_GetError());
        return false;
    }

    SDL_Color color = { 170, 255, 195, 255 };

    SDL_Surface *s = TTF_RenderText_Blended_Wrapped(font, "Hello, SDL3_ttf!\nПривет, SDL3_ttf!", 0, color, 0);

    if (s)
    {
        text_texture = createTextureFromSurface(g_gpu.device, g_gpu.queue, s);

        text_sprite.scale[0] = (float)text_texture.width;
        text_sprite.scale[1] = (float)text_texture.height;

        SDL_DestroySurface(s);
    }

    LogApp(">>> Loading shared texture asset...");

    crate_texture = createTexture(g_gpu.device, g_gpu.queue, ASSET_PATH("images/crate.webp"));

    LogApp(">>> Creating Aligned Uniform Buffer");

    WGPUBufferDescriptor buffer_desc;
    SDL_memset(&buffer_desc, 0, sizeof(buffer_desc));
    buffer_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    buffer_desc.size = UNIFORM_ALIGNMENT * NUM_OBJECTS;

    uniform_buffer = wgpuDeviceCreateBuffer(g_gpu.device, &buffer_desc);

    WGPUBindGroupLayoutEntry bgl_entries[3];
    SDL_memset(bgl_entries, 0, sizeof(bgl_entries));

    bgl_entries[0].binding = 0;
    bgl_entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    bgl_entries[0].buffer.type = WGPUBufferBindingType_Uniform;
    bgl_entries[0].buffer.hasDynamicOffset = true;
    bgl_entries[0].buffer.minBindingSize = sizeof(Uniforms);

    bgl_entries[1].binding = 1;
    bgl_entries[1].visibility = WGPUShaderStage_Fragment;
    bgl_entries[1].texture.sampleType = WGPUTextureSampleType_Float;
    bgl_entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;

    bgl_entries[2].binding = 2;
    bgl_entries[2].visibility = WGPUShaderStage_Fragment;
    bgl_entries[2].sampler.type = WGPUSamplerBindingType_Filtering;

    WGPUBindGroupLayoutDescriptor bgl_desc;
    SDL_memset(&bgl_desc, 0, sizeof(bgl_desc));
    bgl_desc.entryCount = 3;
    bgl_desc.entries = bgl_entries;

    bind_group_layout = wgpuDeviceCreateBindGroupLayout(g_gpu.device, &bgl_desc);

    /* Crate bind group */
    WGPUBindGroupEntry crate_bg_entries[3];
    SDL_memset(crate_bg_entries, 0, sizeof(crate_bg_entries));

    crate_bg_entries[0].binding = 0;
    crate_bg_entries[0].buffer = uniform_buffer;
    crate_bg_entries[0].size = sizeof(Uniforms);

    crate_bg_entries[1].binding = 1;
    crate_bg_entries[1].textureView = crate_texture.view;

    crate_bg_entries[2].binding = 2;
    crate_bg_entries[2].sampler = crate_texture.sampler;

    WGPUBindGroupDescriptor crate_bg_desc;
    SDL_memset(&crate_bg_desc, 0, sizeof(crate_bg_desc));
    crate_bg_desc.layout = bind_group_layout;
    crate_bg_desc.entryCount = 3;
    crate_bg_desc.entries = crate_bg_entries;

    crate_bind_group = wgpuDeviceCreateBindGroup(g_gpu.device, &crate_bg_desc);

    /* Text bind group */
    WGPUBindGroupEntry text_bg_entries[3];
    SDL_memset(text_bg_entries, 0, sizeof(text_bg_entries));

    text_bg_entries[0].binding = 0;
    text_bg_entries[0].buffer = uniform_buffer;
    text_bg_entries[0].size = sizeof(Uniforms);

    text_bg_entries[1].binding = 1;
    text_bg_entries[1].textureView = text_texture.view;

    text_bg_entries[2].binding = 2;
    text_bg_entries[2].sampler = text_texture.sampler;

    WGPUBindGroupDescriptor text_bg_desc;
    SDL_memset(&text_bg_desc, 0, sizeof(text_bg_desc));
    text_bg_desc.layout = bind_group_layout;
    text_bg_desc.entryCount = 3;
    text_bg_desc.entries = text_bg_entries;

    text_bind_group = wgpuDeviceCreateBindGroup(g_gpu.device, &text_bg_desc);

    /* Pipeline layout */
    WGPUPipelineLayoutDescriptor pipeline_layout_desc;
    SDL_memset(&pipeline_layout_desc, 0, sizeof(pipeline_layout_desc));
    pipeline_layout_desc.bindGroupLayoutCount = 1;
    pipeline_layout_desc.bindGroupLayouts = &bind_group_layout;

    WGPUPipelineLayout pipeline_layout = wgpuDeviceCreatePipelineLayout(g_gpu.device, &pipeline_layout_desc);

    LogApp(">>> Creating crate render pipeline");
    pipeline = CreatePipeline(shader_code, pipeline_layout);

    LogApp(">>> Creating text render pipeline");
    text_pipeline = CreatePipeline(text_shader_code, pipeline_layout);

    wgpuPipelineLayoutRelease(pipeline_layout);

    return pipeline != NULL && text_pipeline != NULL;
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
#ifndef __EMSCRIPTEN__

    if (!SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "60"))
    {
        SDL_Log("Failed to set a frame rate: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

#endif

    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");

    if (!SDL_Init(SDL_INIT_VIDEO) || !TTF_Init())
    {
        return SDL_APP_FAILURE;
    }

    window = SDL_CreateWindow(
        "integrate-sdl3-ttf-and-sdl-image-webgpu-cpp",
        WIN_WIDTH,
        WIN_HEIGHT,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);

    if (!window)
    {
        return SDL_APP_FAILURE;
    }

    if (!InitWebGPUContext(&g_gpu, window))
    {
        return SDL_APP_FAILURE;
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    if (event->type == SDL_EVENT_QUIT)
    {
        return SDL_APP_SUCCESS;
    }

    if (event->type == SDL_EVENT_WINDOW_RESIZED || event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
    {
        g_gpu.need_reconfigure = true;
    }

    if (event->type == SDL_EVENT_WINDOW_DISPLAY_CHANGED || event->type == SDL_EVENT_DID_ENTER_FOREGROUND)
    {
        g_gpu.need_recreate_surface = true;
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    if (g_gpu.instance)
    {
        wgpuInstanceProcessEvents(g_gpu.instance);
    }

    if (!g_gpu.device)
    {
        return SDL_APP_CONTINUE;
    }

    bool was_configured = g_gpu.is_configured;

    ReconfigureSurfaceIfNeeded(&g_gpu);

    if (!was_configured && g_gpu.is_configured)
    {
        if (!InitPipeline())
        {
            return SDL_APP_FAILURE;
        }
    }

    WGPUSurfaceTexture surfaceTexture;
    SDL_memset(&surfaceTexture, 0, sizeof(surfaceTexture));

    wgpuSurfaceGetCurrentTexture(g_gpu.surface, &surfaceTexture);

#if defined(WGPUSurfaceGetCurrentTextureStatus_Success) && defined(WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)

#define SURFACE_STATUS_SUCCESS(s) \
    ((s) == WGPUSurfaceGetCurrentTextureStatus_Success || (s) == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)

#elif defined(WGPUSurfaceGetCurrentTextureStatus_SuccessStatus)

#define SURFACE_STATUS_SUCCESS(s) ((s) == WGPUSurfaceGetCurrentTextureStatus_SuccessStatus)

#else

#define SURFACE_STATUS_SUCCESS(s) ((s) == 0 || (s) == 1)

#endif

    if (!SURFACE_STATUS_SUCCESS(surfaceTexture.status))
    {
        return SDL_APP_CONTINUE;
    }

    int w_pixels = 0;
    int h_pixels = 0;

    SDL_GetWindowSizeInPixels(window, &w_pixels, &h_pixels);

    if (w_pixels <= 0 || h_pixels <= 0)
    {
        return SDL_APP_CONTINUE;
    }

    mat4 proj;

    float half_w = (float)w_pixels * 0.5f;
    float half_h = (float)h_pixels * 0.5f;

    glm_ortho(-half_w, half_w, half_h, -half_h, -1.0f, 1.0f, proj);

    float display_scale = SDL_GetWindowDisplayScale(window);

    if (display_scale <= 0.0f)
    {
        display_scale = 1.0f;
    }

    WGPUTextureView view = wgpuTextureCreateView(surfaceTexture.texture, NULL);
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(g_gpu.device, NULL);

    WGPUColor clear_color = { 0.15, 0.15, 0.18, 1.0 };
    WGPURenderPassColorAttachment colorAttachment;
    SDL_memset(&colorAttachment, 0, sizeof(colorAttachment));
    colorAttachment.view = view;
    colorAttachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    colorAttachment.loadOp = WGPULoadOp_Clear;
    colorAttachment.storeOp = WGPUStoreOp_Store;
    colorAttachment.clearValue = clear_color;

    WGPURenderPassDescriptor renderPassDesc;
    SDL_memset(&renderPassDesc, 0, sizeof(renderPassDesc));
    renderPassDesc.colorAttachmentCount = 1;
    renderPassDesc.colorAttachments = &colorAttachment;

    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &renderPassDesc);

    /* Draw crates */
    wgpuRenderPassEncoderSetPipeline(pass, pipeline);

    vec3 axis_z = { 0.0f, 0.0f, 1.0f };

    for (int i = 0; i < NUM_CRATES; i++)
    {
        vec3 scaled_pos = {
            crate_sprites[i].pos[0] * display_scale,
            crate_sprites[i].pos[1] * display_scale,
            0.0f
        };

        vec3 scaled_size = {
            crate_sprites[i].scale[0] * display_scale,
            crate_sprites[i].scale[1] * display_scale,
            1.0f
        };

        mat4 model = GLM_MAT4_IDENTITY_INIT;

        glm_translate(model, scaled_pos);
        glm_rotate(model, glm_rad(crate_sprites[i].rotation), axis_z);
        glm_scale(model, scaled_size);

        Uniforms uniforms;

        glm_mat4_mul(proj, model, uniforms.mvp);

        uint32_t offset = i * UNIFORM_ALIGNMENT;

        wgpuQueueWriteBuffer(g_gpu.queue, uniform_buffer, offset, &uniforms, sizeof(Uniforms));

        wgpuRenderPassEncoderSetBindGroup(pass, 0, crate_bind_group, 1, &offset);
        wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);
    }

    /* Draw text */
    wgpuRenderPassEncoderSetPipeline(pass, text_pipeline);

    {
        vec3 text_pos = {
            text_sprite.pos[0] * display_scale,
            text_sprite.pos[1] * display_scale,
            0.0f
        };

        vec3 text_size = {
            text_sprite.scale[0] * display_scale,
            text_sprite.scale[1] * display_scale,
            1.0f
        };

        mat4 model = GLM_MAT4_IDENTITY_INIT;

        glm_translate(model, text_pos);

        glm_rotate(
            model,
            glm_rad(text_sprite.rotation),
            axis_z);

        glm_scale(model, text_size);

        Uniforms uniforms;

        glm_mat4_mul(proj, model, uniforms.mvp);

        uint32_t text_offset = NUM_CRATES * UNIFORM_ALIGNMENT;

        wgpuQueueWriteBuffer(g_gpu.queue, uniform_buffer, text_offset, &uniforms, sizeof(Uniforms));

        wgpuRenderPassEncoderSetBindGroup(pass, 0, text_bind_group, 1, &text_offset);
        wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);
    }

    wgpuRenderPassEncoderEnd(pass);

    WGPUCommandBuffer commandBuffer = wgpuCommandEncoderFinish(encoder, NULL);

    wgpuQueueSubmit(g_gpu.queue, 1, &commandBuffer);

#ifndef __EMSCRIPTEN__

    wgpuSurfacePresent(g_gpu.surface);

#endif

    if (commandBuffer)
    {
        wgpuCommandBufferRelease(commandBuffer);
    }

    if (pass)
    {
        wgpuRenderPassEncoderRelease(pass);
    }

    if (encoder)
    {
        wgpuCommandEncoderRelease(encoder);
    }

    if (view)
    {
        wgpuTextureViewRelease(view);
    }

    if (surfaceTexture.texture)
    {
        wgpuTextureRelease(surfaceTexture.texture);
    }

#undef SURFACE_STATUS_SUCCESS

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    destroyWebGPUTexture(&crate_texture);
    destroyWebGPUTexture(&text_texture);

    if (font)
    {
        TTF_CloseFont(font);
    }

    TTF_Quit();

    if (crate_bind_group)
    {
        wgpuBindGroupRelease(crate_bind_group);
    }

    if (text_bind_group)
    {
        wgpuBindGroupRelease(text_bind_group);
    }

    if (bind_group_layout)
    {
        wgpuBindGroupLayoutRelease(bind_group_layout);
    }

    if (uniform_buffer)
    {
        wgpuBufferRelease(uniform_buffer);
    }

    if (pipeline)
    {
        wgpuRenderPipelineRelease(pipeline);
    }

    if (text_pipeline)
    {
        wgpuRenderPipelineRelease(text_pipeline);
    }

    DestroyWebGPUContext(&g_gpu);

    if (window)
    {
        SDL_DestroyWindow(window);
    }

    SDL_Quit();
}
