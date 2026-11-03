/* opengothic-rs: physics trace format (task 6.2). Written by common/physics/phystrace.cpp
 * when the game runs with `--physics-trace <path>`; read by og-parity
 * (`crates/og-parity/src/phys_trace.rs`). Without the flag nothing is opened or written.
 *
 * Only constants live here: the trace is a file format, not a call interface.
 *
 * Layout, all little-endian, floats as IEEE-754 f32 bits, positions in centimetres (the
 * engine's units, before CollisionWorld::toMeters):
 *
 *   file    = magic "OGPT" (4 bytes), u32 version, record*
 *   record  = u8 kind, u32 body length, body
 *   vec3    = 3 f32
 *   mat4    = 16 f32, column-major as Tempest::Matrix4x4 stores them
 *   str     = u16 length (OG_PT_STR_NULL: a null pointer), bytes (no terminator)
 *   payload = u32: the trace's number of an engine object an object points at (Npc,
 *             Item, Interactive, editor payload), 0 for null. Numbers come from the
 *             pointer (1, 2, ... in first-seen order) and are handed to the physics object
 *             by an OBJ_SET PAYLOAD record, so a replay knows which body is which NPC.
 *   land    = vec3 v, vec3 n, u8 material group, u8 hasCol, f32 hitFraction, str sector,
 *             payload vob, payload uptr                            (RayLandResult)
 *   coll    = vec3 partial, vec3 normal, u8 preFall, payload vob, payload npc,
 *             f32 npcCol, u8 landCol                               (CollisionTest)
 *
 * Every body starts with u32 world: the trace's own number of the DynamicWorld (1, 2, ...
 * in creation order). Objects get trace ids (1, 2, ... per trace, never reused) when they
 * are added; ids of different kinds share one counter.
 *
 * Only outermost public calls are recorded: what a public call does through other public
 * calls (landRay -> ray, tryMove -> setPosition, tick -> bullet moves) is not a record of
 * its own, so a replay calls exactly the recorded functions. A query's or tick's record
 * is written when it returns, a setter's when it is called (setters run no callbacks). Game code that physics calls back (bullet and bbox callbacks,
 * the item hit sound) counts as outermost again: its records sit between CALLBACK_BEGIN
 * and CALLBACK_END, written before the record of the call that ran the callback (a
 * replay applies them when it reaches that call's callback). Calls from several threads
 * are serialised in the order they finish. Not recorded: plain getters (NpcItem::center,
 * position, groundOffset, ...), the static rayBox / materialFriction / materialDensity
 * (no world state), validateSectorName, debugDraw. */
#pragma once

#include <stdint.h>

#define OG_PT_MAGIC   "OGPT"
#define OG_PT_VERSION 1u
#define OG_PT_STR_NULL 0xFFFFu

/* Record kinds (u8). */
enum {
  /* u32 world, str world name (null without a World, e.g. an offline oracle),
   * u32 vertices, u32 land triangles, u32 water triangles, u32 sectors,
   * u64 land mesh hash, u64 water mesh hash (FNV-1a over vertex xyz, indices and each
   * segment's material group and sector name; see PhysicVbo::ogHash) */
  OG_PT_WORLD_CREATE = 1,
  /* u32 world */
  OG_PT_WORLD_DESTROY = 2,
  /* u32 world, u32 object, u8 object kind (OG_PT_OBJ_*), then by kind:
   *   NPC:          vec3 bbox min, vec3 bbox max (addGhostObj's input to the capsule;
   *                 both zero without a skeleton)
   *   STATIC/MOVABLE: u32 shape (trace id per PhysicMeshShape), f32 friction,
   *                 u32 triangles, u64 shape hash (PhysicVbo::ogHash), mat4 m
   *   DYNAMIC:      mat4 m, vec3 bounds min, vec3 bounds max, u8 material group
   *   BULLET:       nothing
   *   BBOX:         vec3 min, vec3 max (zenkit AABB)
   *   CAPSULE:      vec3 pos, f32 R
   * An add that returns an empty object (null shape) is not recorded. A shape's first use
   * is preceded by its SHAPE record. */
  OG_PT_OBJ_ADD = 3,
  /* u32 world, u32 object */
  OG_PT_OBJ_DELETE = 4,
  /* u32 world, u32 object, u8 op (OG_PT_SET_*), then by op:
   *   POSITION, BULLET_MOVE, BULLET_POSITION, BULLET_DIRECTION: vec3
   *   ROTATION, BULLET_RANGE: f32
   *   ENABLE: u8
   *   MATRIX: mat4
   *   BULLET_SPELL: i32
   *   PAYLOAD: u8 payload kind (OG_PT_PAYLOAD_*), payload */
  OG_PT_OBJ_SET = 5,
  /* u32 world, u8 query (OG_PT_Q_*), u64 wall time in ns, inputs, outputs (below) */
  OG_PT_QUERY = 6,
  /* u32 world, u64 dt in ms, u64 wall time in ns */
  OG_PT_TICK = 7,
  /* u32 world (the one adding it first), u32 shape, u64 hash (PhysicVbo::ogHash),
   * u32 vertices, vertices as vec3 in metres (Bullet's units, CollisionWorld::toMeters),
   * u32 segments, per segment: u8 material group, str sector, u32 triangles,
   * 3 u32 vertex indices per triangle (Bullet's winding). A shape number is reused only
   * for the same pointer with the same hash; a changed hash gets a new number. */
  OG_PT_SHAPE = 8,
  /* u32 world, u8 callback (OG_PT_CB_*), u32 object (the bullet or bbox the callback
   * is about; 0 for ITEM_HIT), then by callback:
   *   BULLET_MOVE, BULLET_STOP: nothing
   *   BULLET_HIT_MATERIAL: u8 material group
   *   BULLET_HIT_NPC: payload npc
   *   BBOX_HIT: u32 bullet object
   *   ITEM_HIT: payload item, u8 material group, f32 impulse, f32 mass */
  OG_PT_CALLBACK_BEGIN = 9,
  /* u32 world, u8 result (BULLET_HIT_NPC: what onCollide returned; 0 otherwise) */
  OG_PT_CALLBACK_END = 10,
  };

/* Callbacks into game code (u8). */
enum {
  OG_PT_CB_BULLET_MOVE        = 1, /* BulletCallback::onMove */
  OG_PT_CB_BULLET_STOP        = 2, /* BulletCallback::onStop */
  OG_PT_CB_BULLET_HIT_MATERIAL = 3, /* BulletCallback::onCollide(material) */
  OG_PT_CB_BULLET_HIT_NPC     = 4, /* BulletCallback::onCollide(Npc&) */
  OG_PT_CB_BBOX_HIT           = 5, /* BBoxCallback::onCollide */
  OG_PT_CB_ITEM_HIT           = 6, /* the item land-hit sound (World::addLandHitEffect) */
  };

/* Payload kinds (u8): what the user pointer of a physics object is. */
enum {
  OG_PT_PAYLOAD_NPC         = 1, /* NpcItem::setUserPointer */
  OG_PT_PAYLOAD_ITEM        = 2, /* Item::setItem */
  OG_PT_PAYLOAD_INTERACTIVE = 3, /* Item::setInteractive */
  OG_PT_PAYLOAD_EDITOR      = 4, /* Item::setPayloadPtr */
  };

/* Object kinds (u8). */
enum {
  OG_PT_OBJ_NPC     = 1,
  OG_PT_OBJ_STATIC  = 2,
  OG_PT_OBJ_MOVABLE = 3,
  OG_PT_OBJ_DYNAMIC = 4,
  OG_PT_OBJ_BULLET  = 5,
  OG_PT_OBJ_BBOX    = 6,
  OG_PT_OBJ_CAPSULE = 7,
  };

/* Object setters (u8). */
enum {
  OG_PT_SET_POSITION         = 1, /* NpcItem::setPosition */
  OG_PT_SET_ROTATION         = 2, /* NpcItem::setRotation (degrees, as passed) */
  OG_PT_SET_ENABLE           = 3, /* NpcItem::setEnable */
  OG_PT_SET_MATRIX           = 4, /* Item::setObjMatrix */
  OG_PT_SET_BULLET_MOVE      = 5, /* BulletBody::move */
  OG_PT_SET_BULLET_POSITION  = 6, /* BulletBody::setPosition */
  OG_PT_SET_BULLET_DIRECTION = 7, /* BulletBody::setDirection */
  OG_PT_SET_BULLET_SPELL     = 8, /* BulletBody::setSpellId */
  OG_PT_SET_BULLET_RANGE     = 9, /* BulletBody::setTargetRange */
  OG_PT_SET_PAYLOAD          = 10, /* user pointer setters */
  };

/* Queries (u8): inputs, then outputs. */
enum {
  OG_PT_Q_LAND_RAY       = 1,  /* vec3 from, f32 maxDy                      -> land */
  OG_PT_Q_LAND_SWEEP     = 2,  /* vec3 from, f32 R, f32 maxDy               -> land */
  OG_PT_Q_WATER_RAY      = 3,  /* vec3 from, f32 stepHeight      -> f32 wdepth, u8 hasCol */
  OG_PT_Q_CAMERA_RAY     = 4,  /* vec3 from, vec3 to                    -> i32 waterCol */
  OG_PT_Q_RAY            = 5,  /* vec3 from, vec3 to                        -> land */
  OG_PT_Q_SWEEP          = 6,  /* vec3 from, vec3 to, f32 R                 -> land */
  OG_PT_Q_RAY_NPC        = 7,  /* vec3 from, vec3 to, payload except   -> land, payload npcHit */
  OG_PT_Q_SOUND_OCCLUSION = 8, /* vec3 from, vec3 to                        -> f32 */
  /* u32 object, vec3 to, vec3 from (the start: pos0, or the body's position for the
   * one-argument overload), coll as passed in              -> u8 ok, coll */
  OG_PT_Q_TEST_MOVE      = 9,
  /* u32 object, vec3 to, vec3 position before, coll as passed in
   *                                   -> u8 move code (MC_*), coll, vec3 position after */
  OG_PT_Q_TRY_MOVE       = 10,
  OG_PT_Q_HAS_COLLISION  = 11, /* u32 object                                -> u8 */
  };
