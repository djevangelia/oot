/**
 * @file z_arms_hook.c
 *
 * Hookshot overlay.
 * Hookshot has two actions: ArmsHook_Action_Wait and ArmsHook_Action_Shoot.
 * ArmsHook_Action_Wait is when non-fired. ArmsHook_Action_Shoot is fired.
 * 
 * Overview of Hookshot flow:
 * - Player_Ranged_FireWeapon sets player->heldActor->parent to NULL, to signal fired weapon.
 * - ArmsHook_Action_Wait then sets up ArmsHook_Action_Shoot, sets the chain timer and resets player to its parent.
 * - ArmsHook_Action_Shoot checks for cancelling fire, decrements the timer, moves the chain forward, and checks for
 * collision. If timer expires or collision, it handles moving the player/struck actor and switching to wait action.
 * - To pull the player, ArmsHook_PullPlayer sets player->actor.parent to NULL, which lets Player_UpdateUpperBody
 * set Player_Action_HookshotFly.
 * - Note that there are two parts to the Hookshot function - player and Hookshot actor - which might not always
 * be synced (see: Hookshot jump, Majora's Mask remote Hookshot). A high Y velocity is almost certainly due to
 * Player_Action_HookshotFly not being set as its setup removes normal movement update (see that function for reference).
 */

 #include "z_arms_hook.h"

#include "libc64/math64.h"
#include "controller.h"
#include "gfx.h"
#include "gfx_setupdl.h"
#include "sfx.h"
#include "sys_math.h"
#include "sys_matrix.h"
#include "play_state.h"
#include "player.h"
#include "z_lib.h"

#include "assets/objects/object_link_boy/object_link_boy.h"

#define FLAGS (ACTOR_FLAG_UPDATE_CULLING_DISABLED | ACTOR_FLAG_DRAW_CULLING_DISABLED)

void ArmsHook_Init(Actor* thisx, PlayState* play);
void ArmsHook_Destroy(Actor* thisx, PlayState* play);
void ArmsHook_Update(Actor* thisx, PlayState* play);
void ArmsHook_Draw(Actor* thisx, PlayState* play);

void ArmsHook_Action_Wait(ArmsHook* this, PlayState* play);
void ArmsHook_Action_Shoot(ArmsHook* this, PlayState* play);

ActorProfile Arms_Hook_Profile = {
    /**/ ACTOR_ARMS_HOOK,
    /**/ ACTORCAT_ITEMACTION,
    /**/ FLAGS,
    /**/ OBJECT_LINK_BOY, // This object dependency makes Hookshot not spawn properly as child. Other items use OBJECT_GAMEPLAY_KEEP. Hookshot data can be moved there or elsewhere to fix.
    /**/ sizeof(ArmsHook),
    /**/ ArmsHook_Init,
    /**/ ArmsHook_Destroy,
    /**/ ArmsHook_Update,
    /**/ ArmsHook_Draw,
};

static ColliderQuadInit sQuadInit = {
    {
        COL_MATERIAL_NONE,
        AT_ON | AT_TYPE_PLAYER,
        AC_NONE,
        OC1_NONE,
        OC2_TYPE_PLAYER,
        COLSHAPE_QUAD,
    },
    {
        ELEM_MATERIAL_UNK2,
        { 0x00000080, HIT_SPECIAL_EFFECT_NONE, 0x01 },
        { 0xFFCFFFFF, HIT_BACKLASH_NONE, 0x00 },
        ATELEM_ON | ATELEM_NEAREST | ATELEM_SFX_NORMAL,
        ACELEM_NONE,
        OCELEM_NONE,
    },
    { { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } } },
};

static Vec3f sUnusedVec1 = { 0.0f, 0.5f, 0.0f };
static Vec3f sUnusedVec2 = { 0.0f, 0.5f, 0.0f };

static Color_RGB8 sUnusedColors[] = {
    { 255, 255, 100 },
    { 255, 255, 50 },
};

static Vec3f posVecInactive = { 0.0f, 0.0f, 0.0f };
static Vec3f posVecActive = { 0.0f, 0.0f, 900.0f };
static Vec3f tipVecInactive = { 0.0f, 500.0f, -3000.0f };
static Vec3f baseVecInactive = { 0.0f, -500.0f, -3000.0f };
static Vec3f tipVecActive = { 0.0f, 500.0f, 1200.0f };
static Vec3f baseVecActive = { 0.0f, -500.0f, 1200.0f };

void ArmsHook_SetupAction(ArmsHook* this, ArmsHookActionFunc actionFunc) {
    this->actionFunc = actionFunc;
}

void ArmsHook_Init(Actor* thisx, PlayState* play) {
    ArmsHook* this = (ArmsHook*)thisx;

    // Hook colliders
    Collider_InitQuad(play, &this->collider);
    Collider_SetQuad(play, &this->collider, &this->actor, &sQuadInit);
    ArmsHook_SetupAction(this, ArmsHook_Action_Wait);
    this->unk_1E8 = this->actor.world.pos;
}

void ArmsHook_Destroy(Actor* thisx, PlayState* play) {
    ArmsHook* this = (ArmsHook*)thisx;

    if (this->attachedActor != NULL) {
        this->attachedActor->flags &= ~ACTOR_FLAG_HOOKSHOT_ATTACHED;
    }

    Collider_DestroyQuad(play, &this->collider);
}

/**
 * Wait for player to fire the Hookshot
 */
void ArmsHook_Action_Wait(ArmsHook* this, PlayState* play) {
    if (this->actor.parent == NULL) { // = Player has fired Hookshot (set in Player_Ranged_FireWeapon)
        Player* player = GET_PLAYER(play);
        // get correct timer length for hookshot or longshot
        s32 length = (player->heldItemAction == PLAYER_IA_HOOKSHOT) ? 13 : 26;

        ArmsHook_SetupAction(this, ArmsHook_Action_Shoot);
        Actor_SetProjectileSpeed(&this->actor, 20.0f);
        this->actor.parent = &GET_PLAYER(play)->actor;
        this->timer = length;
    }
}

/**
 * Start pulling Player so he flies toward the hookshot's current location.
 * Setting Player's parent pointer indicates that he should begin flying.
 * See `Player_UpdateUpperBody` and `Player_Action_HookshotFly` for Player's side of the interation.
 */
void ArmsHook_PullPlayer(ArmsHook* this) {
    this->actor.child = this->actor.parent;
    this->actor.parent->parent = &this->actor;
}

/**
 * Reset player's child and heldActor to Hookshot (= unfired state),
 * and reset player's parent and Hookshot's child to NULL (= remove pull state)
 */
s32 ArmsHook_AttachToPlayer(ArmsHook* this, Player* player) {
    player->actor.child = &this->actor;
    player->heldActor = &this->actor;
    if (this->actor.child != NULL) {
        player->actor.parent = NULL;
        this->actor.child = NULL;
        return true;
    }
    return false;
}

/**
 * Detach from a struck actor.
 */
void ArmsHook_DetachFromActor(ArmsHook* this) {
    if (this->attachedActor != NULL) {
        this->attachedActor->flags &= ~ACTOR_FLAG_HOOKSHOT_ATTACHED;
        this->attachedActor = NULL;
    }
}

/**
 * Check for factors that must cancel chain firing. (Button input is handled in ArmsHook_Action_Shoot)
 * @return 1 if cancel, but this return value is not used (timer 0 is signal)
 */
s32 ArmsHook_CheckForCancel(ArmsHook* this) {
    Player* player = (Player*)this->actor.parent;

    if (Player_HoldsHookshot(player)) {
        if ((player->itemAction != player->heldItemAction) || (player->actor.flags & ACTOR_FLAG_TALK) ||
            ((player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_KNOCKBACK_FROZEN)))) {
            this->timer = 0;
            ArmsHook_DetachFromActor(this);
            Math_Vec3f_Copy(&this->actor.world.pos, &player->rightHandPos);
            return 1;
        }
    }
    return 0;
}

/**
 * Attach to a struck actor. Save offset between actor position and hook position.
 */
void ArmsHook_AttachToActor(ArmsHook* this, Actor* actor) {
    actor->flags |= ACTOR_FLAG_HOOKSHOT_ATTACHED;
    this->attachedActor = actor;
    Math_Vec3f_Diff(&actor->world.pos, &this->actor.world.pos, &this->attachPointOffset);
}

/**
 * Firing the Hookshot chain, checking for and handling collisions with scene and actors,
 * check for fire cancel, pulling player or actor.
 */
void ArmsHook_Action_Shoot(ArmsHook* this, PlayState* play) {
    Player* player = GET_PLAYER(play);

    // Destroy unheld Hookshot
    if ((this->actor.parent == NULL) || (!Player_HoldsHookshot(player))) {
        ArmsHook_DetachFromActor(this);
        Actor_Kill(&this->actor);
        return;
    }

    Actor_PlaySfx_Flagged2(&player->actor, NA_SE_IT_HOOKSHOT_CHAIN - SFX_FLAG);
    ArmsHook_CheckForCancel(this);

    // If AT hit something before chain timer expired, check if player or actor should be pulled
    if ((this->timer != 0) && (this->collider.base.atFlags & AT_HIT) &&
        (this->collider.elem.atHitElem->elemMaterial != ELEM_MATERIAL_UNK4)) {
        Actor* touchedActor = this->collider.base.at;

        if ((touchedActor->update != NULL) &&
            (touchedActor->flags & (ACTOR_FLAG_HOOKSHOT_PULLS_ACTOR | ACTOR_FLAG_HOOKSHOT_PULLS_PLAYER))) {
            if (this->collider.elem.atHitElem->acElemFlags & ACELEM_HOOKABLE) {
                ArmsHook_AttachToActor(this, touchedActor);

                if (ACTOR_FLAGS_CHECK_ALL(touchedActor, ACTOR_FLAG_HOOKSHOT_PULLS_PLAYER)) {
                    ArmsHook_PullPlayer(this);
                }
            }
        }
        this->timer = 0;
        SFX_PLAY_AT_POS(&this->actor.projectedPos, NA_SE_IT_ARROW_STICK_CRE);
        return;
    }

    // Decrement chain timer until chain is fully extended (or AT hit/cancel).
    // This part handles retraction of the chain and moving player/actor until
    // completion, then sets ArmsHook_Action_Wait again.
    if (DECR(this->timer) == 0) {
        Actor* attachedActor;
        Vec3f handHookDistVec;
        Vec3f newPos;
        f32 handHookDist;
        f32 phi_f16;
        s32 pad1;
        f32 curActorOffsetXYZ;
        f32 attachPointOffsetXYZ;
        f32 velocity;

        attachedActor = this->attachedActor;

        if (attachedActor != NULL) {
            if ((attachedActor->update == NULL) ||
                !ACTOR_FLAGS_CHECK_ALL(attachedActor, ACTOR_FLAG_HOOKSHOT_ATTACHED)) {
                attachedActor = NULL;
                this->attachedActor = NULL;
            } else if (this->actor.child != NULL) {
                curActorOffsetXYZ = Actor_WorldDistXYZToActor(&this->actor, attachedActor);
                attachPointOffsetXYZ = sqrtf(SQ(this->attachPointOffset.x) + SQ(this->attachPointOffset.y) +
                                             SQ(this->attachPointOffset.z));

                // Keep the hookshot actor at the same relative offset as the initial attachment even if the actor moves
                Math_Vec3f_Diff(&attachedActor->world.pos, &this->attachPointOffset, &this->actor.world.pos);

                // If the actor the hookshot is attached to is moving, the hookshot's current relative
                // position will be different than the initial attachment position.
                // If the distance between those two points is larger than 50 units, detach the hookshot.
                if ((curActorOffsetXYZ - attachPointOffsetXYZ) > 50.0f) {
                    ArmsHook_DetachFromActor(this);
                    attachedActor = NULL;
                }
            }
        }

        handHookDist = Math_Vec3f_DistXYZAndStoreDiff(&player->rightHandPos, &this->actor.world.pos, &handHookDistVec);

        if (handHookDist < 30.0f) {
            velocity = 0.0f;
            phi_f16 = 0.0f;
        } else {
            if (this->actor.child != NULL) {
                velocity = 30.0f;
            } else if (attachedActor != NULL) {
                velocity = 50.0f;
            } else { // Nothing hit - quick retraction
                velocity = 200.0f;
            }
            phi_f16 = handHookDist - velocity;
            if (handHookDist <= velocity) {
                phi_f16 = 0.0f;
            }
            velocity = phi_f16 / handHookDist;
        }

        newPos.x = handHookDistVec.x * velocity;
        newPos.y = handHookDistVec.y * velocity;
        newPos.z = handHookDistVec.z * velocity;

        // Not pulling Player
        if (this->actor.child == NULL) {
            // If attached to Water Temple opening fish lock
            if ((attachedActor != NULL) && (attachedActor->id == ACTOR_BG_SPOT06_OBJECTS)) {
                Math_Vec3f_Diff(&attachedActor->world.pos, &this->attachPointOffset, &this->actor.world.pos);
                phi_f16 = 1.0f;
            } else {
                // Reposition Hookshot to player hand (newPos used as zero vector)
                Math_Vec3f_Sum(&player->rightHandPos, &newPos, &this->actor.world.pos);
                if (attachedActor != NULL) {
                    Math_Vec3f_Sum(&this->actor.world.pos, &this->attachPointOffset, &attachedActor->world.pos);
                }
            }
        // Pulling Player - set new player velocity for XYZ and rotate
        // Player position is updated in Player_Action_HookshotFly
        } else {
            Math_Vec3f_Diff(&handHookDistVec, &newPos, &player->actor.velocity);
            player->actor.world.rot.x =
                Math_Atan2S(sqrtf(SQ(handHookDistVec.x) + SQ(handHookDistVec.z)), -handHookDistVec.y);
        }

        // Finalizing, return to wait action
        if (phi_f16 < 50.0f) {
            ArmsHook_DetachFromActor(this);
            if (phi_f16 == 0.0f) {
                ArmsHook_SetupAction(this, ArmsHook_Action_Wait);
                if (ArmsHook_AttachToPlayer(this, player)) {
                    Math_Vec3f_Diff(&this->actor.world.pos, &player->actor.world.pos, &player->actor.velocity);
                    player->actor.velocity.y -= 20.0f;
                }
            }
        }
    } else {
        // If timer isn't zero, move the hook forward and line check for collision
        CollisionPoly* poly;
        s32 bgId;
        Vec3f intersectPos;
        Vec3f prevFrameDiff;
        Vec3f sp60;

        Actor_MoveXZGravity(&this->actor);
        Math_Vec3f_Diff(&this->actor.world.pos, &this->actor.prevPos, &prevFrameDiff);
        Math_Vec3f_Sum(&this->unk_1E8, &prevFrameDiff, &this->unk_1E8);
        this->actor.shape.rot.x = Math_Atan2S(this->actor.speed, -this->actor.velocity.y);
        sp60.x = this->unk_1F4.x - (this->unk_1E8.x - this->unk_1F4.x);
        sp60.y = this->unk_1F4.y - (this->unk_1E8.y - this->unk_1F4.y);
        sp60.z = this->unk_1F4.z - (this->unk_1E8.z - this->unk_1F4.z);
        if (BgCheck_EntityLineTest1(&play->colCtx, &sp60, &this->unk_1E8, &intersectPos, &poly, true, true, true, true,
                                    &bgId) &&
            !Actor_HitJabuSurface(play, &this->actor, poly, bgId, &intersectPos)) {
            f32 polyNormalX = COLPOLY_GET_NORMAL(poly->normal.x);
            f32 polyNormalZ = COLPOLY_GET_NORMAL(poly->normal.z);
            s32 pad;

            Math_Vec3f_Copy(&this->actor.world.pos, &intersectPos);
            this->actor.world.pos.x += 10.0f * polyNormalX;
            this->actor.world.pos.z += 10.0f * polyNormalZ;
            this->timer = 0;
            // Collision with hookshotable scene collision or dynapoly
            if (SurfaceType_CanHookshot(&play->colCtx, poly, bgId)) {
                DynaPolyActor* dynaPolyActor;

                if (bgId != BGCHECK_SCENE) {
                    dynaPolyActor = DynaPoly_GetActor(&play->colCtx, bgId);

                    if (dynaPolyActor != NULL) {
                        ArmsHook_AttachToActor(this, &dynaPolyActor->actor);
                    }
                }
                ArmsHook_PullPlayer(this);
                SFX_PLAY_AT_POS(&this->actor.projectedPos, NA_SE_IT_HOOKSHOT_STICK_OBJ);
            // Non-hookshotable collision
            } else {
                CollisionCheck_SpawnShieldParticlesMetal(play, &this->actor.world.pos);
                SFX_PLAY_AT_POS(&this->actor.projectedPos, NA_SE_IT_HOOKSHOT_REFLECT);
            }
        // Cancel chain fire
        } else if (CHECK_BTN_ANY(play->state.input[0].press.button,
                                 (BTN_A | BTN_B | BTN_R | BTN_CUP | BTN_CDOWN | BTN_CLEFT | BTN_CRIGHT))) {
            this->timer = 0;
        }
    }
}

void ArmsHook_Update(Actor* thisx, PlayState* play) {
    ArmsHook* this = (ArmsHook*)thisx;

    this->actionFunc(this, play);
    this->unk_1F4 = this->unk_1E8;
}

void ArmsHook_Draw(Actor* thisx, PlayState* play) {
    s32 pad;
    ArmsHook* this = (ArmsHook*)thisx;
    Player* player = GET_PLAYER(play);
    Vec3f handHookDistVec;
    Vec3f hookTipPos;
    Vec3f hookBasePos;
    f32 handHookDist;
    f32 handHookDistSQ;

    if ((player->actor.draw != NULL) && (player->rightHandType == PLAYER_MODELTYPE_RH_HOOKSHOT)) {
        OPEN_DISPS(play->state.gfxCtx, "../z_arms_hook.c", 850);

        if (1) {}

        // Get current positions for hook, tip and base.
        // Hook position used for collision testing (in Shoot), tip and base for collider
        if ((ArmsHook_Action_Shoot != this->actionFunc) || (this->timer <= 0)) {
            Matrix_MultVec3f(&posVecInactive, &this->unk_1E8);
            Matrix_MultVec3f(&tipVecInactive, &hookTipPos);
            Matrix_MultVec3f(&baseVecInactive, &hookBasePos);
            this->weaponInfo.active = false; // Don't set AT collider. See Player_UpdateWeaponInfo
        } else {
            Matrix_MultVec3f(&posVecActive, &this->unk_1E8);
            Matrix_MultVec3f(&tipVecActive, &hookTipPos);
            Matrix_MultVec3f(&baseVecActive, &hookBasePos);
        }

        // Hook
        Player_UpdateWeaponInfo(play, &this->collider, &this->weaponInfo, &hookTipPos, &hookBasePos);
        Gfx_SetupDL_25Opa(play->state.gfxCtx);
        MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, play->state.gfxCtx, "../z_arms_hook.c", 895);
        gSPDisplayList(POLY_OPA_DISP++, gLinkAdultHookshotTipDL);

        // Chain
        Matrix_Translate(this->actor.world.pos.x, this->actor.world.pos.y, this->actor.world.pos.z, MTXMODE_NEW);
        Math_Vec3f_Diff(&player->rightHandPos, &this->actor.world.pos, &handHookDistVec);
        handHookDistSQ = SQ(handHookDistVec.x) + SQ(handHookDistVec.z);
        handHookDist = sqrtf(handHookDistSQ);
        Matrix_RotateY(Math_FAtan2F(handHookDistVec.x, handHookDistVec.z), MTXMODE_APPLY);
        Matrix_RotateX(Math_FAtan2F(-handHookDistVec.y, handHookDist), MTXMODE_APPLY);
        Matrix_Scale(0.015f, 0.015f, sqrtf(SQ(handHookDistVec.y) + handHookDistSQ) * 0.01f, MTXMODE_APPLY);
        MATRIX_FINALIZE_AND_LOAD(POLY_OPA_DISP++, play->state.gfxCtx, "../z_arms_hook.c", 910);
        gSPDisplayList(POLY_OPA_DISP++, gLinkAdultHookshotChainDL);

        CLOSE_DISPS(play->state.gfxCtx, "../z_arms_hook.c", 913);
    }
}
