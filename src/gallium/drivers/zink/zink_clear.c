/*
 * Copyright 2018 Collabora Ltd.
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
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "zink_context.h"
#include "zink_resource.h"
#include "util/format/u_format.h"
#include "util/u_framebuffer.h"
#include "util/format_srgb.h"
#include "util/u_rect.h"

static inline bool
check_3d_layers(struct pipe_surface *psurf)
{
   /* SPEC PROBLEM:
    * though the vk spec doesn't seem to explicitly address this, currently drivers
    * are claiming that all 3D images have a single "3D" layer regardless of layercount,
    * so we can never clear them if we aren't trying to clear only layer 0
    */
   if (psurf->u.tex.first_layer)
      return false;
      
   if (psurf->u.tex.last_layer - psurf->u.tex.first_layer > 0)
      return false;
   return true;
}

static void
clear_in_rp(struct pipe_context *pctx,
           unsigned buffers,
           const struct pipe_scissor_state *scissor_state,
           const union pipe_color_union *pcolor,
           double depth, unsigned stencil)
{
   struct zink_context *ctx = zink_context(pctx);
   struct pipe_framebuffer_state *fb = &ctx->fb_state;

   struct zink_batch *batch = zink_batch_rp(ctx);

   VkClearAttachment attachments[1 + PIPE_MAX_COLOR_BUFS];
   int num_attachments = 0;

   if (buffers & PIPE_CLEAR_COLOR) {
      VkClearColorValue color;
      color.float32[0] = pcolor->f[0];
      color.float32[1] = pcolor->f[1];
      color.float32[2] = pcolor->f[2];
      color.float32[3] = pcolor->f[3];

      for (unsigned i = 0; i < fb->nr_cbufs; i++) {
         if (!(buffers & (PIPE_CLEAR_COLOR0 << i)) || !fb->cbufs[i])
            continue;

         attachments[num_attachments].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
         attachments[num_attachments].colorAttachment = i;
         attachments[num_attachments].clearValue.color = color;
         ++num_attachments;
      }
   }

   if (buffers & PIPE_CLEAR_DEPTHSTENCIL && fb->zsbuf) {
      VkImageAspectFlags aspect = 0;
      if (buffers & PIPE_CLEAR_DEPTH)
         aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
      if (buffers & PIPE_CLEAR_STENCIL)
         aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

      attachments[num_attachments].aspectMask = aspect;
      attachments[num_attachments].clearValue.depthStencil.depth = depth;
      attachments[num_attachments].clearValue.depthStencil.stencil = stencil;
      ++num_attachments;
   }

   VkClearRect cr = {};
   if (scissor_state) {
      cr.rect.offset.x = scissor_state->minx;
      cr.rect.offset.y = scissor_state->miny;
      cr.rect.extent.width = MIN2(fb->width, scissor_state->maxx - scissor_state->minx);
      cr.rect.extent.height = MIN2(fb->height, scissor_state->maxy - scissor_state->miny);
   } else {
      cr.rect.extent.width = fb->width;
      cr.rect.extent.height = fb->height;
   }
   cr.baseArrayLayer = 0;
   cr.layerCount = util_framebuffer_get_num_layers(fb);
   vkCmdClearAttachments(batch->cmdbuf, num_attachments, attachments, 1, &cr);
}

static struct zink_batch *
get_clear_batch(struct zink_context *ctx, unsigned width, unsigned height, struct u_rect *region)
{
   struct u_rect intersect = {0, width, 0, height};

   /* FIXME: this is very inefficient; if no renderpass has been started yet,
    * we should record the clear if it's full-screen, and apply it as we
    * start the render-pass. Otherwise we can do a partial out-of-renderpass
    * clear.
    */
   if (!u_rect_test_intersection(region, &intersect))
      /* is this even a thing? */
      return zink_batch_rp(ctx);

    u_rect_find_intersection(region, &intersect);
    if (intersect.x0 != 0 || intersect.y0 != 0 ||
        intersect.x1 != width || intersect.y1 != height)
       return zink_batch_rp(ctx);

   return zink_curr_batch(ctx);
}

void
zink_clear(struct pipe_context *pctx,
           unsigned buffers,
           const struct pipe_scissor_state *scissor_state,
           const union pipe_color_union *pcolor,
           double depth, unsigned stencil)
{
   struct zink_context *ctx = zink_context(pctx);
   struct pipe_framebuffer_state *fb = &ctx->fb_state;
   struct zink_batch *batch;

   if (scissor_state) {
      struct u_rect scissor = {scissor_state->minx, scissor_state->maxx, scissor_state->miny, scissor_state->maxy};
      batch = get_clear_batch(ctx, fb->width, fb->height, &scissor);
   } else
      batch = zink_curr_batch(ctx);


   if (batch->in_rp || ctx->render_condition_active) {
      clear_in_rp(pctx, buffers, scissor_state, pcolor, depth, stencil);
      return;
   }

   VkImageSubresourceRange range = {};
   range.levelCount = 1;
   if (buffers & PIPE_CLEAR_COLOR) {
      range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      for (unsigned i = 0; i < fb->nr_cbufs; i++) {
         if (!(buffers & (PIPE_CLEAR_COLOR0 << i)) || !fb->cbufs[i])
            continue;
          VkClearColorValue color;
          struct pipe_surface *psurf = fb->cbufs[i];

          if (psurf->texture->target == PIPE_TEXTURE_3D && !check_3d_layers(psurf)) {
             clear_in_rp(pctx, buffers, scissor_state, pcolor, depth, stencil);
             return;
          }

          struct zink_resource *res = zink_resource(psurf->texture);
          if (res->layout != VK_IMAGE_LAYOUT_GENERAL && res->layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
             zink_resource_barrier(batch->cmdbuf, res, range.aspectMask, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
          if (psurf->format != res->base.format &&
              !util_format_is_srgb(psurf->format) && util_format_is_srgb(res->base.format)) {
             /* if SRGB mode is disabled for the fb with a backing srgb image then we have to
              * convert this to srgb color
              */
             color.float32[0] = util_format_srgb_to_linear_float(pcolor->f[0]);
             color.float32[1] = util_format_srgb_to_linear_float(pcolor->f[1]);
             color.float32[2] = util_format_srgb_to_linear_float(pcolor->f[2]);
          } else {
             color.float32[0] = pcolor->f[0];
             color.float32[1] = pcolor->f[1];
             color.float32[2] = pcolor->f[2];
          }
          color.float32[3] = pcolor->f[3];
          range.baseMipLevel = psurf->u.tex.level;
          range.baseArrayLayer = psurf->u.tex.first_layer;
          range.layerCount = psurf->u.tex.last_layer - psurf->u.tex.first_layer + 1;

          vkCmdClearColorImage(batch->cmdbuf, res->image, res->layout, &color, 1, &range);
      }
   }

   range.aspectMask = 0;
   if (buffers & PIPE_CLEAR_DEPTHSTENCIL && fb->zsbuf) {
      if (fb->zsbuf->texture->target == PIPE_TEXTURE_3D && !check_3d_layers(fb->zsbuf)) {
         clear_in_rp(pctx, buffers, scissor_state, pcolor, depth, stencil);
         return;
      }

      if (buffers & PIPE_CLEAR_DEPTH)
         range.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
      if (buffers & PIPE_CLEAR_STENCIL)
         range.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
      VkClearDepthStencilValue zs_value = {depth, stencil};
      range.baseMipLevel = fb->zsbuf->u.tex.level;
      range.baseArrayLayer = fb->zsbuf->u.tex.first_layer;
      range.layerCount = fb->zsbuf->u.tex.last_layer - fb->zsbuf->u.tex.first_layer + 1;

      struct zink_resource *res = zink_resource(fb->zsbuf->texture);
      if (res->layout != VK_IMAGE_LAYOUT_GENERAL && res->layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
         zink_resource_barrier(batch->cmdbuf, res, range.aspectMask, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      vkCmdClearDepthStencilImage(batch->cmdbuf, res->image, res->layout, &zs_value, 1, &range);
   }
}