# id Tech 4 code in this engine

This engine is based on ioquake3 and also uses code from the id Tech 4 (Doom 3)
GPL source release:

- Source: https://github.com/id-Software/DOOM-3
- Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.
- License: GNU General Public License version 3 (COPYING-GPLv3.txt), with the additional terms in
  DOOM-3's README.txt.

ioquake3 and the OpenArena gamecode are licensed "GPL version 2 or (at your
option) any later version", so they combine with GPLv3 code; once id Tech 4
code is included, the engine as a whole is distributed under the GPLv3.

No Doom 3 game data (maps, models, textures, sounds) is used, only code.

## Rules for adapted code

1. Keep id Software's copyright and license header on every file that contains
   id Tech 4 code, adapted or not.
2. Under that header, name the DOOM-3 file(s) the code came from (for example
   `Adapted from DOOM-3 neo/game/Mover.cpp`) and say what was changed (ported
   to C, ARB program rewritten as GLSL ES 3.00, and so on).
3. Add a row to the table below in the same commit.

## Files

| Engine file | From DOOM-3 | Changes |
| --- | --- | --- |
| code/idlib_lite/Str.h, Str.cpp | neo/idlib/Str.h, Str.cpp | plain malloc instead of the block allocator; no color table or memory report |
| code/idlib_lite/Lexer.h, Lexer.cpp | neo/idlib/Lexer.h, Lexer.cpp | files read through the engine file system; static punctuation tables; a newline after a block comment is counted |
| code/idlib_lite/Token.h, Token.cpp | neo/idlib/Token.h, Token.cpp | include only |
| code/idlib_lite/Parser.h, Parser.cpp | neo/idlib/Parser.h, Parser.cpp | __DATE__/__TIME__ are fixed strings (determinism); "/" path separator |
| code/idlib_lite/HashIndex.h, HashIndex.cpp, List.h, StaticList.h, StrList.h | neo/idlib/containers/ | unchanged (include only) |
| code/idlib_lite/idlib_lite.h, Math_lite.h, Lib_lite.cpp | neo/idlib/Lib.h, Lib.cpp, precompiled.h, math/Math.h, Vector.h/.cpp, Angles.h/.cpp, sys/sys_public.h | only what the script VM uses; idLib::common forwards to the engine and its Error longjmps; sin/cos/atan2 are the engine's deterministic musl functions, exact 1/sqrt |
| code/idscript/Script_Event.h, Script_Event.cpp | neo/game/gamesys/Event.h, Event.cpp | idEventDef only, as a run-time table the engine and the game module fill; no idEvent queue |
| code/idscript/Script_Program.h, Script_Program.cpp | neo/game/script/Script_Program.h, .cpp | entities are int handles; 4-byte entity/object slots on every build; errors longjmp; no save games |
| code/idscript/Script_Compiler.h, Script_Compiler.cpp | neo/game/script/Script_Compiler.h, .cpp | errors longjmp; sys/entity event checks against the run-time event table |
| code/idscript/Script_Interpreter.h, Script_Interpreter.cpp | neo/game/script/Script_Interpreter.h, .cpp | game events become call records for the game module (the pump); errors and runaway loops kill the thread, not the game; per-frame instruction limit |
| code/idscript/Script_Thread.h, Script_Thread.cpp | neo/game/script/Script_Thread.h, .cpp | not an idClass; engine thread events in a table; scheduling by (time, post order) instead of the idEvent queue; game events and waitFor through the game module |
| code/idscript/Script_Local.h | neo/game/Game_local.h | only program, time and printers of idGameLocal |
| tests/romdev/lib/oaxtraj.mjs | neo/idlib/math/Interpolate.h, neo/idlib/math/Extrapolate.h (by way of oa-gamecode bg_oax_traj.c) | JS test evaluator: the accel/decel phases in closed form, in doubles, plus the packed-duration decoding |
| code/idgui/Window.cpp, Window.h | neo/ui/Window.cpp, Window.h | Namespace idgui; 64-bit safe expression ops; type tags instead of dynamic_cast; no decl tables, editor or skipped window types (parsed as plain windows) |
| code/idgui/SimpleWindow.cpp, SimpleWindow.h | neo/ui/SimpleWindow.cpp, SimpleWindow.h | Namespace idgui; 64-bit safe member offsets |
| code/idgui/ChoiceWindow.cpp, ChoiceWindow.h | neo/ui/ChoiceWindow.cpp, ChoiceWindow.h | Namespace idgui; no string tables; never binds engine cvars |
| code/idgui/SliderWindow.cpp, SliderWindow.h | neo/ui/SliderWindow.cpp, SliderWindow.h | Namespace idgui; never binds engine cvars |
| code/idgui/ListWindow.cpp, ListWindow.h | neo/ui/ListWindow.cpp, ListWindow.h | Namespace idgui; synthetic click built in place |
| code/idgui/EditWindow.cpp, EditWindow.h | neo/ui/EditWindow.cpp, EditWindow.h | Namespace idgui; fixed console keys; never binds engine cvars |
| code/idgui/GuiScript.cpp, GuiScript.h | neo/ui/GuiScript.cpp, GuiScript.h | Namespace idgui; type tags; localSound through the client; endGame disabled |
| code/idgui/Winvar.cpp, Winvar.h | neo/ui/Winvar.cpp, Winvar.h | Namespace idgui; type tags (IsType, WinVarCast); delete[] fix |
| code/idgui/RegExp.cpp, RegExp.h | neo/ui/RegExp.cpp, RegExp.h | Namespace idgui; type tags; no string tables |
| code/idgui/UserInterface.cpp, UserInterface.h, UserInterfaceLocal.h | neo/ui/UserInterface.cpp, UserInterface.h, UserInterfaceLocal.h | Namespace idgui; manager trimmed to what world GUIs use |
| code/idgui/DeviceContext.cpp, DeviceContext.h | neo/ui/DeviceContext.cpp, DeviceContext.h; font structures from neo/renderer/RenderSystem.h | Draws through Q3 SetColor / StretchPic (+ DrawQuad) into a render target; glyphs built from OA bitmap fonts; OA assets |
| code/idgui/Rectangle.h | neo/ui/Rectangle.h | Namespace idgui |
| code/idgui/idgui_sys.h | neo/sys/sys_public.h, neo/framework/KeyInput.h, CVarSystem.h, neo/renderer/Material.h | Only the types the windows use; module-private cvars; materials are Q3 shader names; no-op save/demo files |
| code/idgui/idlib/Str.cpp, Str.h | neo/idlib/Str.cpp, Str.h | Namespace idgui; no block allocator or libc-redefining macros; 32-bit sign helper |
| code/idgui/idlib/Lexer.cpp, Lexer.h, Parser.cpp, Parser.h, Token.cpp, Token.h | neo/idlib/Lexer.cpp, Lexer.h, Parser.cpp, Parser.h, Token.cpp, Token.h | Namespace idgui; engine filesystem; 32-bit integers; fixed __DATE__/__TIME__ |
| code/idgui/idlib/List.h, HashIndex.cpp, HashIndex.h, HashTable.h | neo/idlib/containers/List.h, HashIndex.cpp, HashIndex.h, HashTable.h | Namespace idgui |
| code/idgui/idlib/Interpolate.h, Extrapolate.h | neo/idlib/math/Interpolate.h, Extrapolate.h | Namespace idgui (trig through the engine musl ports) |
| code/idgui/idlib/Lib.h, Lib.cpp, Math.h, Dict.h | neo/idlib/Lib.h, Lib.cpp, Dict.h, Dict.cpp, math/Math.h, Math.cpp, Vector.h, Vector.cpp, Matrix.h, Rotation.cpp | Lite rewrites: only what the GUI port uses; dictionary as ordered idStr pairs; errors longjmp to a guard |
| code/qcommon/cm_guisurf.c | neo/renderer/tr_guisurf.cpp (R_SurfaceToTextureAxis), neo/renderer/RenderWorld.cpp (GuiTrace) | C over Q3 BSP draw surfaces flagged SURF_OAX_GUI; ray-triangle hit in the entity frame |
| code/renderergl2/tr_matexpr.c | neo/renderer/Material.cpp (ParseTerm, ParseExpressionPriority, EmitOp, GetExpressionConstant, GetExpressionTemporary, EvaluateRegisters, stage shorthands and light material keywords); neo/framework/DeclTable.cpp (Parse, TableLookup) | Ported to C; Q3 shader-line lexer; registers per shader; tables from scripts/<name>.table; results written to stage constant colors |
| code/renderergl2/tr_ulight.c | neo/game/Light.cpp (ParseSpawnArgsToRenderLight); neo/renderer/tr_lightrun.cpp (R_SetLightProject, R_SetLightFrustum, R_DeriveLightData); neo/renderer/tr_light.cpp (R_ClippedLightScissorRectangle); neo/renderer/tr_polytope.cpp (R_PolytopeSurface); neo/renderer/Image_init.cpp (R_QuadraticImage, R_CreateNoFalloffImage, R_FlatNormalImage, R_WhiteImage, R_BlackImage) | Ported to C on renderergl2 data; planes as vec4_t; interactions from the BSP (light PVS and volume) instead of area references; Q3 light keys converted; new _pointlight and _spotlight images |
| code/renderergl2/tb_ulight_stencil.c | neo/renderer/tr_stencilshadow.cpp (R_AddSilEdges, PointsOrdered); neo/renderer/draw_common.cpp (RB_StencilShadowPass) | Ported to C; world casters are BSP brushes; MD3 silhouettes from welded edge adjacency; finite extrusion with a far-plane clamp in the vertex stage instead of infinite projection; quad winding fixed against the caster centroid |
| code/renderergl2/tr_image_program.c | neo/renderer/Image_program.cpp (image programs), neo/renderer/Image_process.cpp (R_Dropsample) | Ported to C: a small tokenizer replaces idLexer, float[3] replaces idVec3, ri.Malloc replaces R_StaticAlloc; loads through renderergl2 R_LoadImage (RGBA8 only); no timestamps or texture depth; idVec3::Normalize uses 1/sqrtf instead of the table InvSqrt (RSqrt kept bit for bit); scale() clamps before the byte conversion; hooked into R_FindImageFile; adds the imageprogram test command. |
