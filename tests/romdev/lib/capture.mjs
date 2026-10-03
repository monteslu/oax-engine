// capture.mjs: the capture profile tests run at.
//
// The engine defaults to r_picmip 0 (full-resolution textures, step 7.5 D).
// Every golden captured before that change was taken at r_picmip 1, so the
// harnesses boot both builds with LEGACY_PICMIP unless a test asks for
// another value (cartShots / nativeShots / runNative / Session.load take
// `picmip`). Tests of new features capture at picmip 0. OA_PICMIP overrides
// the legacy value for a whole run. Resolution is fixed: native passes
// 1280x720, and a deterministic cart run always renders at its 1280x720
// default whatever the host prefers.
export const LEGACY_PICMIP = Number(process.env.OA_PICMIP ?? 1);
export const CAPTURE_WIDTH = 1280;
export const CAPTURE_HEIGHT = 720;

// native command-line arguments for a capture profile
// picmip null: leave r_picmip at the engine default
export function nativeCaptureArgs({ picmip = LEGACY_PICMIP, width = CAPTURE_WIDTH, height = CAPTURE_HEIGHT } = {}) {
  return ['+set', 'r_mode', '-1', '+set', 'r_customwidth', String(width), '+set', 'r_customheight', String(height),
    '+set', 'r_fullscreen', '0', ...(picmip === null ? [] : ['+set', 'r_picmip', String(picmip)])];
}

// the cart's boot cvars (console_cmd written before the first frame)
// picmip null: no boot command (the engine default)
export function cartBootCommand({ picmip = LEGACY_PICMIP } = {}) {
  return picmip === null ? '' : `set r_picmip ${picmip}`;
}
