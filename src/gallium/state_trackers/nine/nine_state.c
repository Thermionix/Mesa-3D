/*
 * Copyright 2011 Joakim Sindholt <opensource@zhasha.com>
 * Copyright 2013 Christoph Bumiller
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE. */

#include "device9.h"
#include "indexbuffer9.h"
#include "surface9.h"
#include "vertexshader9.h"
#include "pixelshader9.h"
#include "nine_pipe.h"
#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "cso_cache/cso_context.h"
#include "util/u_math.h"

#define DBG_CHANNEL DBG_DEVICE

static void
update_framebuffer(struct NineDevice9 *device)
{
    struct pipe_context *pipe = device->pipe;
    struct nine_state *state = &device->state;
    struct pipe_surface *surf;
    struct pipe_framebuffer_state *fb = &device->state.fb;
    unsigned i;

    DBG("\n");

    fb->nr_cbufs = 0;

    for (i = 0; i < device->caps.NumSimultaneousRTs; ++i) {
        if (state->rt[i]) {
            fb->cbufs[i] = state->rt[i]->surface;
            fb->nr_cbufs = i + 1;
        } else {
            /* Color outputs must match RT slot,
             * drivers will have to handle NULL entries for GL, too.
             */
            fb->cbufs[i] = NULL;
        }
    }
    fb->zsbuf = state->ds ? state->ds->surface : NULL;

    surf = fb->nr_cbufs ? fb->cbufs[fb->nr_cbufs - 1] : fb->zsbuf;

    if (surf) {
        fb->width = surf->width;
        fb->height = surf->height;
    } else {
        fb->width = fb->height = 0;
    }

    pipe->set_framebuffer_state(pipe, fb); /* XXX: cso ? */
}

static void
update_viewport(struct NineDevice9 *device)
{
   struct pipe_context *pipe = device->pipe;
   const D3DVIEWPORT9 *vport = &device->state.viewport;
   struct pipe_viewport_state pvport;

    /* XXX:
     * I hope D3D clip coordinates are still
     * -1 .. +1 for X,Y and
     *  0 .. +1 for Z (use pipe_rasterizer_state.clip_halfz)
     */
    pvport.scale[0] = (float)vport->Width * 0.5f;
    pvport.scale[1] = (float)vport->Height * 0.5f;
    pvport.scale[2] = vport->MaxZ - vport->MinZ;
    pvport.scale[3] = 1.0f;
    pvport.translate[0] = pvport.scale[0] + (float)vport->X;
    pvport.translate[1] = pvport.scale[1] + (float)vport->Y;
    pvport.translate[2] = vport->MinZ;
    pvport.translate[3] = 0.0f;

    pipe->set_viewport_state(pipe, &pvport);
}

static INLINE void
update_scissor(struct NineDevice9 *device)
{
    struct pipe_context *pipe = device->pipe;

    pipe->set_scissor_state(pipe, &device->state.scissor);
}

static INLINE void
update_blend(struct NineDevice9 *device)
{
    nine_convert_blend_state(device->cso, device->state.rs);
}

static INLINE void
update_dsa(struct NineDevice9 *device)
{
    nine_convert_dsa_state(device->cso, device->state.rs);
}

static INLINE void
update_rasterizer(struct NineDevice9 *device)
{
    nine_convert_rasterizer_state(device->cso, device->state.rs);
}

static INLINE void
update_vertex_elements(struct NineDevice9 *device)
{
}

/* OK, this is a bit ugly ... */
static void
update_constants(struct NineDevice9 *device, unsigned shader_type)
{
    struct pipe_context *pipe = device->pipe;
    struct pipe_resource *buf;
    struct pipe_box box;
    const void *data;
    const float *const_f;
    const int *const_i;
    const BOOL *const_b;
    uint32_t data_b[NINE_MAX_CONST_B];
    uint32_t b_true;
    uint16_t dirty_i;
    uint16_t dirty_b;
    uint32_t *dirty_f;
    const unsigned usage = PIPE_TRANSFER_WRITE | PIPE_TRANSFER_DISCARD_RANGE;
    unsigned i, j, c, n;
    unsigned x;

    if (shader_type == PIPE_SHADER_VERTEX) {
        buf = device->constbuf_vs;
        dirty_f = device->state.changed.vs_const_f;
        const_f = device->state.vs_const_f;
        dirty_i = device->state.changed.vs_const_i;
        const_i = &device->state.vs_const_i[0][0];
        dirty_b = device->state.changed.vs_const_b;
        const_b = device->state.vs_const_b;
        b_true = device->vs_bool_true;
    } else {
        buf = device->constbuf_ps;
        dirty_f = device->state.changed.ps_const_f;
        const_f = device->state.ps_const_f;
        dirty_i = device->state.changed.ps_const_i;
        const_i = &device->state.ps_const_i[0][0];
        dirty_b = device->state.changed.ps_const_b;
        const_b = device->state.ps_const_b;
        b_true = device->ps_bool_true;
    }
    box.y = 0;
    box.z = 0;
    box.height = 1;
    box.depth = 1;

    /* write range from min to max changed, it's not much data */
    /* bool1 */
    if (dirty_b) {
       c = util_last_bit(dirty_b);
       i = ffs(dirty_b) - 1;
       x = buf->width0 - (NINE_MAX_CONST_B - i) * 4;
       c -= i;
       for (n = 0; n < c; ++n, ++i)
          data_b[n] = const_b[i] ? b_true : 0;
       box.x = x;
       box.width = n * 4;
       pipe->transfer_inline_write(pipe, buf, 0, usage, &box, data_b, 0, 0);
          
    }

    /* int4 */
    for (c = 0, i = 0; dirty_i; i++, dirty_i >>= 1) {
        if (dirty_i & 1) {
            if (!c)
                x = i;
            ++c;
        } else
        if (c) {
            data = &const_i[x * 4];
            box.x = x * 4 * sizeof(int);
            box.width = c * 4 * sizeof(int);
            c = 0;
            pipe->transfer_inline_write(pipe, buf, 0, usage, &box, data, 0, 0);
        }
    }
    if (c) {
        data = &const_i[x * 4];
        box.x = x * 4 * sizeof(int);
        box.width = c * 4 * sizeof(int);
        pipe->transfer_inline_write(pipe, buf, 0, usage, &box, data, 0, 0);
    }

    /* float4 */
    for (c = 0, i = 0; i < (NINE_MAX_CONST_F + 31) / 32; ++i) {
        uint32_t m = dirty_f[i];

        for (j = 0; m; j++, m >>= 1) {
            if (m & 1) {
               if (!c)
                   x = i * 32 + j;
               ++c;
            } else
            if (c) {
                data = &const_f[x * 4];
                box.x = x * 4 * sizeof(float);
                box.width = c * 4 * sizeof(float);
                c = 0;
                pipe->transfer_inline_write(pipe,
                                            buf, 0, usage, &box, data, 0, 0);
            }
        }
    }
    if (c) {
        data = &const_f[x * 4];
        box.x = x * 4 * sizeof(float);
        box.width = c * 4 * sizeof(float);
        pipe->transfer_inline_write(pipe, buf, 0, usage, &box, data, 0, 0);
    }
}

static void
update_vertex_buffers(struct NineDevice9 *device)
{
    struct pipe_context *pipe = device->pipe;
    struct nine_state *state = &device->state;
    uint32_t mask = state->changed.vtxbuf;
    unsigned i;
    unsigned start;
    unsigned count = 0;

    for (i = 0; mask; mask >>= 1, ++i) {
        if (mask & 1) {
            if (!count)
                start = i;
            ++count;
        } else {
            if (count)
                pipe->set_vertex_buffers(pipe,
                                         start, count, &state->vtxbuf[start]);
            count = 0;
        }
    }
    state->changed.vtxbuf = 0;
}

static INLINE void
update_index_buffer(struct NineDevice9 *device)
{
    struct pipe_context *pipe = device->pipe;
    if (device->state.idxbuf)
        pipe->set_index_buffer(pipe, &device->state.idxbuf->buffer);
    else
        pipe->set_index_buffer(pipe, NULL);
}

#define NINE_STATE_FREQ_GROUP_0 \
   (NINE_STATE_FB |             \
    NINE_STATE_VIEWPORT |       \
    NINE_STATE_SCISSOR |        \
    NINE_STATE_BLEND |          \
    NINE_STATE_DSA |            \
    NINE_STATE_RASTERIZER |     \
    NINE_STATE_VS |             \
    NINE_STATE_PS |             \
    NINE_STATE_BLEND_COLOR |    \
    NINE_STATE_SAMPLE_MASK)

#define NINE_STATE_FREQ_GROUP_1 ~NINE_STATE_FREQ_GROUP_0
boolean
nine_update_state(struct NineDevice9 *device)
{
    struct pipe_context *pipe = device->pipe;
    struct nine_state *state = &device->state;

    DBG("changed state groups: %x | %x\n",
        state->changed.group & NINE_STATE_FREQ_GROUP_0,
        state->changed.group & NINE_STATE_FREQ_GROUP_1);

    if (state->changed.group & NINE_STATE_FREQ_GROUP_0) {
        if (state->changed.group & NINE_STATE_FB)
            update_framebuffer(device);
        if (state->changed.group & NINE_STATE_VIEWPORT)
            update_viewport(device);
        if (state->changed.group & NINE_STATE_SCISSOR)
            update_scissor(device);

        if (state->changed.group & NINE_STATE_BLEND)
            update_blend(device);
        if (state->changed.group & NINE_STATE_DSA)
            update_dsa(device);
        if (state->changed.group & NINE_STATE_RASTERIZER)
            update_rasterizer(device);

        if (state->changed.group & NINE_STATE_VS)
            pipe->bind_vs_state(pipe, device->state.vs->cso);
        if (state->changed.group & NINE_STATE_PS)
            pipe->bind_fs_state(pipe, device->state.ps->cso);

        if (state->changed.group & NINE_STATE_BLEND_COLOR) {
            struct pipe_blend_color color;
            d3dcolor_to_rgba(&color.color[0], state->rs[D3DRS_BLENDFACTOR]);
            pipe->set_blend_color(pipe, &color);
        }
        if (state->changed.group & NINE_STATE_SAMPLE_MASK) {
            pipe->set_sample_mask(pipe, state->rs[D3DRS_MULTISAMPLEMASK]);
        }
    }

    if (state->changed.ucp)
        pipe->set_clip_state(pipe, &state->clip);

    if (state->changed.group & NINE_STATE_FREQ_GROUP_1) {
        if (state->changed.group & NINE_STATE_IDXBUF)
            update_index_buffer(device);

        if ((state->changed.group & (NINE_STATE_VDECL | NINE_STATE_VS)) ||
            state->changed.stream_freq & ~1)
            update_vertex_elements(device);

        if (state->changed.group & NINE_STATE_VS_CONST)
            update_constants(device, PIPE_SHADER_VERTEX);
        if (state->changed.group & NINE_STATE_PS_CONST)
            update_constants(device, PIPE_SHADER_FRAGMENT);
    }
    if (state->changed.vtxbuf)
        update_vertex_buffers(device);

    device->state.changed.group = 0;

    return TRUE;
}


static const DWORD nine_render_state_defaults[NINED3DRS_LAST + 1] =
{
 /* [D3DRS_ZENABLE] = D3DZB_TRUE; wine: auto_depth_stencil */
    [D3DRS_ZENABLE] = D3DZB_FALSE,
    [D3DRS_FILLMODE] = D3DFILL_SOLID,
    [D3DRS_SHADEMODE] = D3DSHADE_GOURAUD,
/*  [D3DRS_LINEPATTERN] = 0x00000000, */
    [D3DRS_ZWRITEENABLE] = TRUE,
    [D3DRS_ALPHATESTENABLE] = FALSE,
    [D3DRS_LASTPIXEL] = TRUE,
    [D3DRS_SRCBLEND] = D3DBLEND_ONE,
    [D3DRS_DESTBLEND] = D3DBLEND_ZERO,
    [D3DRS_CULLMODE] = D3DCULL_CCW,
    [D3DRS_ZFUNC] = D3DCMP_LESSEQUAL,
    [D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS,
    [D3DRS_ALPHAREF] = 0,
    [D3DRS_DITHERENABLE] = FALSE,
    [D3DRS_ALPHABLENDENABLE] = FALSE,
    [D3DRS_FOGENABLE] = FALSE,
    [D3DRS_SPECULARENABLE] = FALSE,
/*  [D3DRS_ZVISIBLE] = 0, */
    [D3DRS_FOGCOLOR] = 0,
    [D3DRS_FOGTABLEMODE] = D3DFOG_NONE,
    [D3DRS_FOGSTART] = 0x00000000,
    [D3DRS_FOGEND] = 0x3F800000,
    [D3DRS_FOGDENSITY] = 0x3F800000,
/*  [D3DRS_EDGEANTIALIAS] = FALSE, */
    [D3DRS_RANGEFOGENABLE] = FALSE,
    [D3DRS_STENCILENABLE] = FALSE,
    [D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP,
    [D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP,
    [D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP,
    [D3DRS_STENCILREF] = 0,
    [D3DRS_STENCILMASK] = 0xFFFFFFFF,
    [D3DRS_STENCILFUNC] = D3DCMP_ALWAYS,
    [D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF,
    [D3DRS_TEXTUREFACTOR] = 0xFFFFFFFF,
    [D3DRS_WRAP0] = 0,
    [D3DRS_WRAP1] = 0,
    [D3DRS_WRAP2] = 0,
    [D3DRS_WRAP3] = 0,
    [D3DRS_WRAP4] = 0,
    [D3DRS_WRAP5] = 0,
    [D3DRS_WRAP6] = 0,
    [D3DRS_WRAP7] = 0,
    [D3DRS_CLIPPING] = TRUE,
    [D3DRS_LIGHTING] = TRUE,
    [D3DRS_AMBIENT] = 0,
    [D3DRS_FOGVERTEXMODE] = D3DFOG_NONE,
    [D3DRS_COLORVERTEX] = TRUE,
    [D3DRS_LOCALVIEWER] = TRUE,
    [D3DRS_NORMALIZENORMALS] = FALSE,
    [D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1,
    [D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2,
    [D3DRS_AMBIENTMATERIALSOURCE] = D3DMCS_MATERIAL,
    [D3DRS_EMISSIVEMATERIALSOURCE] = D3DMCS_MATERIAL,
    [D3DRS_VERTEXBLEND] = D3DVBF_DISABLE,
    [D3DRS_CLIPPLANEENABLE] = 0,
/*  [D3DRS_SOFTWAREVERTEXPROCESSING] = FALSE, */
    [D3DRS_POINTSIZE] = 0x3F800000,
    [D3DRS_POINTSIZE_MIN] = 0x3F800000,
    [D3DRS_POINTSPRITEENABLE] = FALSE,
    [D3DRS_POINTSCALEENABLE] = FALSE,
    [D3DRS_POINTSCALE_A] = 0x3F800000,
    [D3DRS_POINTSCALE_B] = 0x00000000,
    [D3DRS_POINTSCALE_C] = 0x00000000,
    [D3DRS_MULTISAMPLEANTIALIAS] = TRUE,
    [D3DRS_MULTISAMPLEMASK] = 0xFFFFFFFF,
    [D3DRS_PATCHEDGESTYLE] = D3DPATCHEDGE_DISCRETE,
/*  [D3DRS_PATCHSEGMENTS] = 0x3F800000, */
    [D3DRS_DEBUGMONITORTOKEN] = 0xDEADCAFE,
    [D3DRS_POINTSIZE_MAX] = 0x3F800000, /* depends on cap */
    [D3DRS_INDEXEDVERTEXBLENDENABLE] = FALSE,
    [D3DRS_COLORWRITEENABLE] = 0x0000000f,
    [D3DRS_TWEENFACTOR] = 0x00000000,
    [D3DRS_BLENDOP] = D3DBLENDOP_ADD,
    [D3DRS_POSITIONDEGREE] = D3DDEGREE_CUBIC,
    [D3DRS_NORMALDEGREE] = D3DDEGREE_LINEAR,
    [D3DRS_SCISSORTESTENABLE] = FALSE,
    [D3DRS_SLOPESCALEDEPTHBIAS] = 0,
    [D3DRS_MINTESSELLATIONLEVEL] = 0x3F800000,
    [D3DRS_MAXTESSELLATIONLEVEL] = 0x3F800000,
    [D3DRS_ANTIALIASEDLINEENABLE] = FALSE,
    [D3DRS_ADAPTIVETESS_X] = 0x00000000,
    [D3DRS_ADAPTIVETESS_Y] = 0x00000000,
    [D3DRS_ADAPTIVETESS_Z] = 0x3F800000,
    [D3DRS_ADAPTIVETESS_W] = 0x00000000,
    [D3DRS_ENABLEADAPTIVETESSELLATION] = FALSE,
    [D3DRS_TWOSIDEDSTENCILMODE] = FALSE,
    [D3DRS_CCW_STENCILFAIL] = D3DSTENCILOP_KEEP,
    [D3DRS_CCW_STENCILZFAIL] = D3DSTENCILOP_KEEP,
    [D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP,
    [D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS,
    [D3DRS_COLORWRITEENABLE1] = 0x0000000F,
    [D3DRS_COLORWRITEENABLE2] = 0x0000000F,
    [D3DRS_COLORWRITEENABLE3] = 0x0000000F,
    [D3DRS_BLENDFACTOR] = 0xFFFFFFFF,
    [D3DRS_SRGBWRITEENABLE] = 0,
    [D3DRS_DEPTHBIAS] = 0,
    [D3DRS_WRAP8] = 0,
    [D3DRS_WRAP9] = 0,
    [D3DRS_WRAP10] = 0,
    [D3DRS_WRAP11] = 0,
    [D3DRS_WRAP12] = 0,
    [D3DRS_WRAP13] = 0,
    [D3DRS_WRAP14] = 0,
    [D3DRS_WRAP15] = 0,
    [D3DRS_SEPARATEALPHABLENDENABLE] = FALSE,
    [D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE,
    [D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO,
    [D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD,
};
void
nine_state_set_defaults(struct nine_state *state, D3DCAPS9 *caps)
{
    memcpy(state->rs, nine_render_state_defaults, sizeof(state->rs));
    state->rs[D3DRS_POINTSIZE_MAX] = fui(caps->MaxPointSize);

    state->changed.group = NINE_STATE_ALL;
}

/*
static const DWORD nine_render_states_pixel[] =
{
    D3DRS_ALPHABLENDENABLE,
    D3DRS_ALPHAFUNC,
    D3DRS_ALPHAREF,
    D3DRS_ALPHATESTENABLE,
    D3DRS_ANTIALIASEDLINEENABLE,
    D3DRS_BLENDFACTOR,
    D3DRS_BLENDOP,
    D3DRS_BLENDOPALPHA,
    D3DRS_CCW_STENCILFAIL,
    D3DRS_CCW_STENCILPASS,
    D3DRS_CCW_STENCILZFAIL,
    D3DRS_COLORWRITEENABLE,
    D3DRS_COLORWRITEENABLE1,
    D3DRS_COLORWRITEENABLE2,
    D3DRS_COLORWRITEENABLE3,
    D3DRS_DEPTHBIAS,
    D3DRS_DESTBLEND,
    D3DRS_DESTBLENDALPHA,
    D3DRS_DITHERENABLE,
    D3DRS_FILLMODE,
    D3DRS_FOGDENSITY,
    D3DRS_FOGEND,
    D3DRS_FOGSTART,
    D3DRS_LASTPIXEL,
    D3DRS_SCISSORTESTENABLE,
    D3DRS_SEPARATEALPHABLENDENABLE,
    D3DRS_SHADEMODE,
    D3DRS_SLOPESCALEDEPTHBIAS,
    D3DRS_SRCBLEND,
    D3DRS_SRCBLENDALPHA,
    D3DRS_SRGBWRITEENABLE,
    D3DRS_STENCILENABLE,
    D3DRS_STENCILFAIL,
    D3DRS_STENCILFUNC,
    D3DRS_STENCILMASK,
    D3DRS_STENCILPASS,
    D3DRS_STENCILREF,
    D3DRS_STENCILWRITEMASK,
    D3DRS_STENCILZFAIL,
    D3DRS_TEXTUREFACTOR,
    D3DRS_TWOSIDEDSTENCILMODE,
    D3DRS_WRAP0,
    D3DRS_WRAP1,
    D3DRS_WRAP10,
    D3DRS_WRAP11,
    D3DRS_WRAP12,
    D3DRS_WRAP13,
    D3DRS_WRAP14,
    D3DRS_WRAP15,
    D3DRS_WRAP2,
    D3DRS_WRAP3,
    D3DRS_WRAP4,
    D3DRS_WRAP5,
    D3DRS_WRAP6,
    D3DRS_WRAP7,
    D3DRS_WRAP8,
    D3DRS_WRAP9,
    D3DRS_ZENABLE,
    D3DRS_ZFUNC,
    D3DRS_ZWRITEENABLE
};
*/
const uint32_t nine_render_states_pixel[(NINED3DRS_LAST + 31) / 32] =
{
   0x00000000, 0x00000000, 0x00000000, 0x00000000,
   0x00000000, 0x00000000, 0x00000000
};

/*
static const DWORD nine_render_states_vertex[] =
{
    D3DRS_ADAPTIVETESS_W,
    D3DRS_ADAPTIVETESS_X,
    D3DRS_ADAPTIVETESS_Y,
    D3DRS_ADAPTIVETESS_Z,
    D3DRS_AMBIENT,
    D3DRS_AMBIENTMATERIALSOURCE,
    D3DRS_CLIPPING,
    D3DRS_CLIPPLANEENABLE,
    D3DRS_COLORVERTEX,
    D3DRS_CULLMODE,
    D3DRS_DIFFUSEMATERIALSOURCE,
    D3DRS_EMISSIVEMATERIALSOURCE,
    D3DRS_ENABLEADAPTIVETESSELLATION,
    D3DRS_FOGCOLOR,
    D3DRS_FOGDENSITY,
    D3DRS_FOGENABLE,
    D3DRS_FOGEND,
    D3DRS_FOGSTART,
    D3DRS_FOGTABLEMODE,
    D3DRS_FOGVERTEXMODE,
    D3DRS_INDEXEDVERTEXBLENDENABLE,
    D3DRS_LIGHTING,
    D3DRS_LOCALVIEWER,
    D3DRS_MAXTESSELLATIONLEVEL,
    D3DRS_MINTESSELLATIONLEVEL,
    D3DRS_MULTISAMPLEANTIALIAS,
    D3DRS_MULTISAMPLEMASK,
    D3DRS_NORMALDEGREE,
    D3DRS_NORMALIZENORMALS,
    D3DRS_PATCHEDGESTYLE,
    D3DRS_POINTSCALE_A,
    D3DRS_POINTSCALE_B,
    D3DRS_POINTSCALE_C,
    D3DRS_POINTSCALEENABLE,
    D3DRS_POINTSIZE,
    D3DRS_POINTSIZE_MAX,
    D3DRS_POINTSIZE_MIN,
    D3DRS_POINTSPRITEENABLE,
    D3DRS_POSITIONDEGREE,
    D3DRS_RANGEFOGENABLE,
    D3DRS_SHADEMODE,
    D3DRS_SPECULARENABLE,
    D3DRS_SPECULARMATERIALSOURCE,
    D3DRS_TWEENFACTOR,
    D3DRS_VERTEXBLEND
};
*/
const uint32_t nine_render_states_vertex[(NINED3DRS_LAST + 31) / 32] =
{
   0x00000000, 0x00000000, 0x00000000, 0x00000000,
   0x00000000, 0x00000000, 0x00000000
};

/* TODO: put in the right values */
const uint32_t nine_render_state_group[NINED3DRS_LAST + 1] =
{
    [D3DRS_ZENABLE] = NINE_STATE_ALL,
    [D3DRS_FILLMODE] = NINE_STATE_ALL,
    [D3DRS_SHADEMODE] = NINE_STATE_ALL,
    [D3DRS_ZWRITEENABLE] = NINE_STATE_ALL,
    [D3DRS_ALPHATESTENABLE] = NINE_STATE_ALL,
    [D3DRS_LASTPIXEL] = NINE_STATE_ALL,
    [D3DRS_SRCBLEND] = NINE_STATE_ALL,
    [D3DRS_DESTBLEND] = NINE_STATE_ALL,
    [D3DRS_CULLMODE] = NINE_STATE_ALL,
    [D3DRS_ZFUNC] = NINE_STATE_ALL,
    [D3DRS_ALPHAREF] = NINE_STATE_ALL,
    [D3DRS_ALPHAFUNC] = NINE_STATE_ALL,
    [D3DRS_DITHERENABLE] = NINE_STATE_ALL,
    [D3DRS_ALPHABLENDENABLE] = NINE_STATE_ALL,
    [D3DRS_FOGENABLE] = NINE_STATE_ALL,
    [D3DRS_SPECULARENABLE] = NINE_STATE_ALL,
    [D3DRS_FOGCOLOR] = NINE_STATE_ALL,
    [D3DRS_FOGTABLEMODE] = NINE_STATE_ALL,
    [D3DRS_FOGSTART] = NINE_STATE_ALL,
    [D3DRS_FOGEND] = NINE_STATE_ALL,
    [D3DRS_FOGDENSITY] = NINE_STATE_ALL,
    [D3DRS_RANGEFOGENABLE] = NINE_STATE_ALL,
    [D3DRS_STENCILENABLE] = NINE_STATE_ALL,
    [D3DRS_STENCILFAIL] = NINE_STATE_ALL,
    [D3DRS_STENCILZFAIL] = NINE_STATE_ALL,
    [D3DRS_STENCILPASS] = NINE_STATE_ALL,
    [D3DRS_STENCILFUNC] = NINE_STATE_ALL,
    [D3DRS_STENCILREF] = NINE_STATE_ALL,
    [D3DRS_STENCILMASK] = NINE_STATE_ALL,
    [D3DRS_STENCILWRITEMASK] = NINE_STATE_ALL,
    [D3DRS_TEXTUREFACTOR] = NINE_STATE_ALL,
    [D3DRS_WRAP0] = NINE_STATE_ALL,
    [D3DRS_WRAP1] = NINE_STATE_ALL,
    [D3DRS_WRAP2] = NINE_STATE_ALL,
    [D3DRS_WRAP3] = NINE_STATE_ALL,
    [D3DRS_WRAP4] = NINE_STATE_ALL,
    [D3DRS_WRAP5] = NINE_STATE_ALL,
    [D3DRS_WRAP6] = NINE_STATE_ALL,
    [D3DRS_WRAP7] = NINE_STATE_ALL,
    [D3DRS_CLIPPING] = NINE_STATE_ALL,
    [D3DRS_LIGHTING] = NINE_STATE_ALL,
    [D3DRS_AMBIENT] = NINE_STATE_ALL,
    [D3DRS_FOGVERTEXMODE] = NINE_STATE_ALL,
    [D3DRS_COLORVERTEX] = NINE_STATE_ALL,
    [D3DRS_LOCALVIEWER] = NINE_STATE_ALL,
    [D3DRS_NORMALIZENORMALS] = NINE_STATE_ALL,
    [D3DRS_DIFFUSEMATERIALSOURCE] = NINE_STATE_ALL,
    [D3DRS_SPECULARMATERIALSOURCE] = NINE_STATE_ALL,
    [D3DRS_AMBIENTMATERIALSOURCE] = NINE_STATE_ALL,
    [D3DRS_EMISSIVEMATERIALSOURCE] = NINE_STATE_ALL,
    [D3DRS_VERTEXBLEND] = NINE_STATE_ALL,
    [D3DRS_CLIPPLANEENABLE] = NINE_STATE_ALL,
    [D3DRS_POINTSIZE] = NINE_STATE_ALL,
    [D3DRS_POINTSIZE_MIN] = NINE_STATE_ALL,
    [D3DRS_POINTSPRITEENABLE] = NINE_STATE_ALL,
    [D3DRS_POINTSCALEENABLE] = NINE_STATE_ALL,
    [D3DRS_POINTSCALE_A] = NINE_STATE_ALL,
    [D3DRS_POINTSCALE_B] = NINE_STATE_ALL,
    [D3DRS_POINTSCALE_C] = NINE_STATE_ALL,
    [D3DRS_MULTISAMPLEANTIALIAS] = NINE_STATE_ALL,
    [D3DRS_MULTISAMPLEMASK] = NINE_STATE_ALL,
    [D3DRS_PATCHEDGESTYLE] = NINE_STATE_ALL,
    [D3DRS_DEBUGMONITORTOKEN] = NINE_STATE_ALL,
    [D3DRS_POINTSIZE_MAX] = NINE_STATE_ALL,
    [D3DRS_INDEXEDVERTEXBLENDENABLE] = NINE_STATE_ALL,
    [D3DRS_COLORWRITEENABLE] = NINE_STATE_ALL,
    [D3DRS_TWEENFACTOR] = NINE_STATE_ALL,
    [D3DRS_BLENDOP] = NINE_STATE_ALL,
    [D3DRS_POSITIONDEGREE] = NINE_STATE_ALL,
    [D3DRS_NORMALDEGREE] = NINE_STATE_ALL,
    [D3DRS_SCISSORTESTENABLE] = NINE_STATE_ALL,
    [D3DRS_SLOPESCALEDEPTHBIAS] = NINE_STATE_ALL,
    [D3DRS_ANTIALIASEDLINEENABLE] = NINE_STATE_ALL,
    [D3DRS_MINTESSELLATIONLEVEL] = NINE_STATE_ALL,
    [D3DRS_MAXTESSELLATIONLEVEL] = NINE_STATE_ALL,
    [D3DRS_ADAPTIVETESS_X] = NINE_STATE_ALL,
    [D3DRS_ADAPTIVETESS_Y] = NINE_STATE_ALL,
    [D3DRS_ADAPTIVETESS_Z] = NINE_STATE_ALL,
    [D3DRS_ADAPTIVETESS_W] = NINE_STATE_ALL,
    [D3DRS_ENABLEADAPTIVETESSELLATION] = NINE_STATE_ALL,
    [D3DRS_TWOSIDEDSTENCILMODE] = NINE_STATE_ALL,
    [D3DRS_CCW_STENCILFAIL] = NINE_STATE_ALL,
    [D3DRS_CCW_STENCILZFAIL] = NINE_STATE_ALL,
    [D3DRS_CCW_STENCILPASS] = NINE_STATE_ALL,
    [D3DRS_CCW_STENCILFUNC] = NINE_STATE_ALL,
    [D3DRS_COLORWRITEENABLE1] = NINE_STATE_ALL,
    [D3DRS_COLORWRITEENABLE2] = NINE_STATE_ALL,
    [D3DRS_COLORWRITEENABLE3] = NINE_STATE_ALL,
    [D3DRS_BLENDFACTOR] = NINE_STATE_ALL,
    [D3DRS_SRGBWRITEENABLE] = NINE_STATE_ALL,
    [D3DRS_DEPTHBIAS] = NINE_STATE_ALL,
    [D3DRS_WRAP8] = NINE_STATE_ALL,
    [D3DRS_WRAP9] = NINE_STATE_ALL,
    [D3DRS_WRAP10] = NINE_STATE_ALL,
    [D3DRS_WRAP11] = NINE_STATE_ALL,
    [D3DRS_WRAP12] = NINE_STATE_ALL,
    [D3DRS_WRAP13] = NINE_STATE_ALL,
    [D3DRS_WRAP14] = NINE_STATE_ALL,
    [D3DRS_WRAP15] = NINE_STATE_ALL,
    [D3DRS_SEPARATEALPHABLENDENABLE] = NINE_STATE_ALL,
    [D3DRS_SRCBLENDALPHA] = NINE_STATE_ALL,
    [D3DRS_DESTBLENDALPHA] = NINE_STATE_ALL,
    [D3DRS_BLENDOPALPHA] = NINE_STATE_ALL
};