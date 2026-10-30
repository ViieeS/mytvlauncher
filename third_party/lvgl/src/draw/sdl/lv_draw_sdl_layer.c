/**
 * @file lv_draw_sdl_refr.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "../../lv_conf_internal.h"

#if LV_USE_GPU_SDL

#include "../../core/lv_refr.h"

#include "lv_draw_sdl.h"
#include "lv_draw_sdl_priv.h"
#include "lv_draw_sdl_composite.h"
#include "lv_draw_sdl_utils.h"
#include "lv_draw_sdl_layer.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_draw_layer_ctx_t * lv_draw_sdl_layer_init(lv_draw_ctx_t * draw_ctx, lv_draw_layer_ctx_t * layer_ctx,
                                             lv_draw_layer_flags_t flags)
{
    lv_draw_sdl_ctx_t * ctx = (lv_draw_sdl_ctx_t *) draw_ctx;
    SDL_Renderer * renderer = ctx->renderer;

    lv_draw_sdl_layer_ctx_t * transform_ctx = (lv_draw_sdl_layer_ctx_t *) layer_ctx;

    transform_ctx->flags = flags;
    transform_ctx->orig_target = SDL_GetRenderTarget(renderer);

    lv_coord_t target_w = lv_area_get_width(&layer_ctx->area_full);
    lv_coord_t target_h = lv_area_get_height(&layer_ctx->area_full);

    enum lv_draw_sdl_composite_texture_id_t texture_id = LV_DRAW_SDL_COMPOSITE_TEXTURE_ID_TRANSFORM0 +
                                                         ctx->internals->transform_count;
    transform_ctx->target = lv_draw_sdl_composite_texture_obtain(ctx, texture_id, target_w, target_h,
                                                                 &transform_ctx->target_in_cache);
    transform_ctx->target_rect.x = 0;
    transform_ctx->target_rect.y = 0;
    transform_ctx->target_rect.w = target_w;
    transform_ctx->target_rect.h = target_h;

    layer_ctx->max_row_with_alpha = target_h;
    layer_ctx->max_row_with_no_alpha = target_h;

    SDL_SetTextureBlendMode(transform_ctx->target, SDL_BLENDMODE_BLEND);
    SDL_SetRenderTarget(renderer, transform_ctx->target);

    /* SDL_RenderClear is not working properly, so we overwrite the target with solid color */
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderFillRect(renderer, NULL);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    /* Set proper drawing context for transform layer */
    ctx->internals->transform_count += 1;
    draw_ctx->buf_area = &layer_ctx->area_full;
    draw_ctx->clip_area = &layer_ctx->area_full;

    return layer_ctx;
}

void lv_draw_sdl_layer_blend(lv_draw_ctx_t * draw_ctx, lv_draw_layer_ctx_t * layer_ctx,
                             const lv_draw_img_dsc_t * draw_dsc)
{
    lv_draw_sdl_ctx_t * ctx = (lv_draw_sdl_ctx_t *) draw_ctx;
    lv_draw_sdl_layer_ctx_t * transform_ctx = (lv_draw_sdl_layer_ctx_t *) layer_ctx;

    SDL_Renderer * renderer = ctx->renderer;

    /* local patch: a transform layer covers only the part of the object being redrawn. Each part
     * was zoomed about the pivot and rounded to whole pixels on its own, so neighbouring parts of
     * one zoomed object landed 1-2 px apart (clipped glyphs, stepped edges). Place every part in
     * float coordinates from the one absolute pivot instead. */
    const lv_area_t * a = (transform_ctx->flags & LV_DRAW_LAYER_FLAG_CAN_SUBDIVIDE) ? &layer_ctx->area_act
                          : &layer_ctx->area_full;
    float z = draw_dsc->zoom / 256.0f;
    /* local patch: a layer inside another layer (a zoomed tile in a fading row) is blended into the
     * outer layer's texture, whose origin is the outer area, not the screen: it landed off target */
    const lv_area_t * dst = ctx->internals->transform_count > 1 ? layer_ctx->original.buf_area : NULL;
    float ox = dst ? dst->x1 : 0, oy = dst ? dst->y1 : 0;
    float px = layer_ctx->area_full.x1 + draw_dsc->pivot.x - ox, py = layer_ctx->area_full.y1 + draw_dsc->pivot.y - oy;
    SDL_FRect trans_rect = {px + (a->x1 - ox - px) * z, py + (a->y1 - oy - py) * z, lv_area_get_width(a) * z,
                            lv_area_get_height(a) * z
                           };

    SDL_SetRenderTarget(renderer, transform_ctx->orig_target);

    /*Render off-screen texture, transformed*/
    SDL_Rect clip_rect;
    lv_area_t clip = *layer_ctx->original.clip_area;
    lv_area_move(&clip, (lv_coord_t) -ox, (lv_coord_t) -oy);
    lv_area_to_sdl_rect(&clip, &clip_rect);
    SDL_FPoint center = {.x = px - trans_rect.x, .y = py - trans_rect.y};
    SDL_RenderSetClipRect(renderer, &clip_rect);
    /* local patch: the layer was drawn with SDL_BLENDMODE_BLEND onto transparent black, so it holds
     * premultiplied colour. Composited with plain BLEND every partial alpha (text edges, .82 text,
     * .35 rings) was applied twice and transformed objects came out faint. */
    static SDL_BlendMode premul;
    if(!premul) premul = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                                                    SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ONE,
                                                    SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);
    SDL_SetTextureBlendMode(transform_ctx->target, premul);
    SDL_SetTextureColorMod(transform_ctx->target, draw_dsc->opa, draw_dsc->opa, draw_dsc->opa);
    SDL_SetTextureAlphaMod(transform_ctx->target, draw_dsc->opa);
    SDL_RenderCopyExF(renderer, transform_ctx->target, &transform_ctx->target_rect, &trans_rect,
                      draw_dsc->angle / 10.0, &center, SDL_FLIP_NONE);
    SDL_SetTextureColorMod(transform_ctx->target, 0xFF, 0xFF, 0xFF);   /* the texture is a shared composite */
    SDL_SetTextureBlendMode(transform_ctx->target, SDL_BLENDMODE_BLEND);
    SDL_RenderSetClipRect(renderer, NULL);
}

void lv_draw_sdl_layer_destroy(lv_draw_ctx_t * draw_ctx, lv_draw_layer_ctx_t * layer_ctx)
{
    lv_draw_sdl_ctx_t * ctx = (lv_draw_sdl_ctx_t *) draw_ctx;
    lv_draw_sdl_layer_ctx_t * transform_ctx = (lv_draw_sdl_layer_ctx_t *) layer_ctx;
    if(!transform_ctx->target_in_cache && transform_ctx->target != NULL) {
        LV_LOG_WARN("Texture is not cached, this will impact performance.");
        SDL_DestroyTexture(transform_ctx->target);
    }
    ctx->internals->transform_count -= 1;
}

void lv_draw_sdl_transform_areas_offset(lv_draw_sdl_ctx_t * ctx, bool has_composite, lv_area_t * apply_area,
                                        lv_area_t * coords, lv_area_t * clip)
{
    if(ctx->internals->transform_count == 0) {
        return;
    }
    lv_area_t * area = ctx->base_draw.buf_area;
    lv_area_move(coords, -area->x1, -area->y1);
    lv_area_move(clip, -area->x1, -area->y1);
    if(has_composite) {
        lv_area_move(apply_area, -area->x1, -area->y1);
    }
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

#endif /*LV_USE_GPU_SDL*/
