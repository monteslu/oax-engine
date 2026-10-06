# Navigation from intent (oax)

Bots on maps without AAS (heightmap terrain, imported maps) path on a
Recast/Detour navigation mesh the server builds from the world's collision
geometry (`code/server/sv_nav_oax.c`, `nav_oax.cpp`, `qcommon/cm_navgeom.c`).
Walking alone does not reach everything a map offers. The map's entities
say where movement goes beyond walking, and the game module (`g_oax_navlinks.c`)
turns them into off-mesh links and cost volumes of that mesh. Nothing here
needs bspc or an AAS file, and none of it changes stock maps: they keep
their AAS and the stock bots.

The navmesh is built for a map when `sv_navmesh` says so (default `-1`: maps
with terrain or without an AAS file). The game adds its links once the level
has settled (jump pads aim one frame after spawning) and the server rebuilds
the mesh with them. `sv_navLinks 0` builds the plain walkable mesh (a test
control).

### How the mesh is built

- **Once, lazily.** The map-load build waits: an oax game module commits
  its links within its first frames (`G_OAX_NAV_COMMIT`), and that is the
  one build; without a commit, the first query builds the plain mesh.
- **Tiles, from the spawn points.** The mesh is Detour-tiled (256 cells a
  side, 2048 units at `sv_navCellSize 8`) over the walkable geometry's
  bounds, with no size cap. Only tiles walkers can reach are built: the
  spawn points' tiles, then breadth first every tile a built tile's
  polygons open onto and every tile an off-mesh link lands in. An imported
  map's sky room or the floor at the bottom of its void costs nothing.
  Teleporters that land more than a tile away are connected by an oax
  patch to Detour (`connectFarOffMeshLinks`).
- **Brushes are solid.** Each world brush is rasterized both as its faces
  and as a filled volume, and a merged span keeps the walkability of the
  face that caps it. Faces buried in other brushes (overlapping detail,
  the stacked convex cells of an imported hull) stop being floors in
  mid-air.
- **Floors need open air.** A walkable face with no open leaf in front of
  it (structural solid, the void) is not walkable, and every walkable span
  whose air above is opaque is dropped. Brushes with no face in open air
  need no fill.
- **Deadly volumes remove floors.** A walkable triangle wholly inside a
  removing cost volume (a `trigger_hurt` that kills on touch, a damage
  zone of 100 or more per second) neither makes tiles nor widens the build.

Debug values: `sv_nav_tiles` of `sv_nav_grid_tiles`, `sv_nav_seeds`,
`sv_nav_volumes`, `sv_nav_closed_faces`, `sv_nav_buried_brushes`,
`sv_nav_opaque_tops`, `sv_nav_forbidden_tris`, `sv_nav_error`.

The mesh is built from world brushes only: brushes that belong to an inline
model (triggers, movers, zones) are not obstacles. q3map2 lists them in the
world leafs too, and OA's `common/trigger` shader has no `nonsolid`, so
without this every trigger would cut a hole in the mesh.

## Entity keys

### trigger_teleport

| Key | Meaning |
| --- | --- |
| `noretrigger 1` | No-retrigger arrival. A player who arrives inside this trigger by teleport (a teleporter, `target_teleporter`, the translocator) is not sent on until he has left it. Default 0: classic Q3 behaviour. |

Some source engines place a teleporter's arrival on the paired teleporter; in Q3 such a pair
ping-pongs, so converters had to move arrivals. With `noretrigger` the
source placement can stay.

Navigation: every `trigger_teleport` (not spectator-only, not a seamless
warp) becomes a one-way link from the floor at the trigger's centre to the
floor under its destination. A destination inside a classic trigger (no
`noretrigger`) gets no link, because the arrival would re-trigger (bspc
drops those too); `g_nav_links_pingpong` counts them.

### trigger_push (jump pads)

No new keys. The link runs from the pad to where the push really lands: the
pad's velocity (set by `AimAtTarget` from `g_gravity`) is flown with the
player's box in 8 ms steps, sliding along walls, until it lands on walkable
ground. bspc instead predicts the arc at 1.1x the horizontal speed, so pads
that work in game could get no reachability.

### func_oax_zone (ladders and damage)

| Key | Meaning |
| --- | --- |
| `ladder <speed>` | The zone is a ladder volume: no gravity inside; forward climbs (down when looking more than 45 degrees down), jump climbs, crouch descends, sideways moves at half speed. Predicted by the cgame like every zone key. |
| `damage <n>` | (existing) damage per second; the zone is also a hazard cost volume. |
| `navcost <c>` | Hazard cost multiplier for a damage zone (default 10; a zone doing 100 or more per second defaults to -1); negative removes the surface from the mesh. |

A ladder zone becomes a one-way link from the floor below the volume to the
highest walkable ground just beside its top (probed on all four sides).

### trigger_hurt

| Key | Meaning |
| --- | --- |
| `navcost <c>` | Cost of walking inside (default 10: a unit inside costs ten units of walking). A trigger that kills on touch (`dmg` >= 100) defaults to -1: removed from the mesh, like a pit. Any negative value removes it. |

Hazards are area costs, not lava: their walkable surface stays on the mesh,
so an item inside a hazard stays reachable and bots only avoid crossing it
when a way around is cheaper. bspc gives every `trigger_hurt` lava contents,
which made such items unroutable. A trigger that starts off (spawnflag 1) is
not a hazard (its state when the links are built counts).

### info_oax_route

A pair of point entities marking movement the game supports by rule, from
this entity to its `target` (another `info_oax_route`). Origins may be a
little above the floor; the floor below each is used.

| Key | Meaning |
| --- | --- |
| `target` | the other end (its `targetname`) |
| `kind` | `jump` (default: a plain jump), `drop` (walk off an edge), `swim` (through water: a dive, an underwater hop, the climb onto a bank; the bot swims toward the end in three dimensions, rising with jump), or `translocator` |
| `bidir 1` | usable both ways (default one way) |
| `radius` | how far from a walkable polygon an end may be (default 32) |

`translocator` routes are rule-gated: bots use them only while
`g_oaxTranslocator` is on. A `swim` end may float in deep water: its floor
is looked for up to 768 units down (the bottom), where other ends get 256.

### Doors (navmesh obstacles)

The navmesh ignores movers, so a shut door is open floor to it. A game
module can mark boxes as obstacles (`G_OAX_NAV_ADDBLOCKER` before the
commit; `G_OAX_NAV_SETBLOCKER` to open and close one). While an obstacle is
on, its box carves the walkable surface, and the tiles it reaches (only the
ones the first build made) are rebuilt when it changes, in a few
milliseconds each; the far landings of off-mesh links are reconnected after
a rebuild. Obstacles are off for the first build, so the floor behind a shut
door is reached and built, and the navmesh hash stays the map's own; they
turn on right after. The oax game marks the doors Assault objectives open
(oax-gamecode docs/assault.md).

## The translocator rule

`g_oaxTranslocator 1` (server info, latched, default 0) gives every player a
translocator in the grappling hook's slot (weapon 10; it replaces the hook
while on). Classic play stays the default (roadmap principle 5).

- Fire with no beacon out throws it from the muzzle at 900 ups along the
  view, with no spread or randomness. It flies under the level's gravity,
  bounces off walls at half speed and stops dead on the first walkable
  surface, so where it lands depends on the throw alone.
- Fire with the beacon out teleports the thrower onto the spot it marks
  (refused, beacon kept, when a player box does not fit). Velocity is
  cleared.
- Telefrags: arriving on a player telefrags him (as teleporters do); a
  beacon damaged by an enemy is disrupted, and porting to it kills the
  thrower (credited to the disruptor); a flag carrier who ports drops the
  flag.
- The beacon goes away when its owner dies, respawns or leaves, when it
  falls into a nodrop volume, and after 20 s.

## Link kinds and syscalls

Obstacles: `G_OAX_NAV_ADDBLOCKER (mins, maxs)` returns the blocker's index
(queued, like a cost volume; on after the commit); `G_OAX_NAV_SETBLOCKER
(index, on)` returns the tiles rebuilt.

Game syscalls 1094-1097 (`code/qcommon/oax.h`, token `nav`):

| Syscall | Arguments |
| --- | --- |
| `G_OAX_NAV_ADDLINK` (1094) | start, end (feet on the floor), kind, radius, bidir |
| `G_OAX_NAV_ADDAREA` (1095) | mins, maxs, cost (negative: remove) |
| `G_OAX_NAV_COMMIT` (1096) | rebuild the mesh with the queued links and volumes |
| `G_OAX_NAV_FINDPATHEX` (1097) | start, goal, points, links per point, max, flags, include, exclude |

Polygon flags: 1 walkable, 2 inside a hazard. Link kinds: 0x10 teleporter,
0x20 jump pad, 0x40 ladder, 0x80 jump route, 0x100 drop route, 0x1000
translocator route. The default filter (and `G_OAX_NAV_FINDPATH`) includes
walking, hazards and every kind below 0x1000; rule-gated kinds must be
included by the caller.

## Debugging

- `nav_path sx sy sz gx gy gz [include [exclude]]` prints a path and sets the
  debug values `nav_path` ("count flags x y z ...", flags & 1 = partial) and
  `nav_path_links` (link indexes the path takes).
- Links that cannot be made (a marker in solid even 48 units up, no floor
  within range below a marker, a pad arc that never lands) are printed and
  listed in `g_nav_links_skipped` ("kind#entity end: reason; ...").
- Debug values: `sv_nav_links` ("connected/submitted"), `sv_nav_links_open`
  (links that did not reach the mesh at both ends), `sv_nav_areas`,
  `sv_nav_hash`; `g_nav_link_<i>` ("kind sx sy sz ex ey ez"),
  `g_nav_links_pingpong`, `g_nav_hazards`; per bot link kind
  `g_navbot_lk_<kind>` ("taken done failed") and `g_navbot_lf_<kind>` (the
  last failure); `g_teleports_<client>`, `g_tele_locks`; `g_tl_throws`,
  `g_tl_ports`, `g_tl_telefrags`, `g_tl_disrupted`.
- `bot_oaxIdle 1` makes navmesh bots stand still and hold fire (a test
  target); `g_navbot_pos_<client>` publishes "x y z health" for the first four.
- Test: `tests/romdev/tests/nav-intent.mjs` on `oax_nav_intent` (and the
  classic control `oax_nav_intent_classic`).
