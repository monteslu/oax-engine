uniform sampler2D u_TextureMap;

uniform vec4      u_Color;

uniform vec2      u_InvTexRes;
varying vec2      var_TexCoords;

// oax: on the direct path the tone map applies the atmosphere itself, so
// the exposure is measured on the fogged image here too (first pass only)
// and the atmosphere (worldspawn oax_atmosphere), as oaxatmos_fp.glsl:
// u_FogEyeT > 0.5 on; u_FogColorMask rgb + density; u_FogDistance falloff,
// base z, sun scatter, sky distance
uniform sampler2D u_ScreenDepthMap;
uniform float     u_FogEyeT;
uniform vec4      u_FogColorMask;
uniform vec4      u_FogDistance;
uniform vec4      u_ViewInfo;
uniform vec4      u_NormalScale;
uniform vec3      u_ViewOrigin;
uniform vec3      u_ViewForward;
uniform vec3      u_ViewLeft;
uniform vec3      u_ViewUp;
uniform vec4      u_PrimaryLightOrigin;

// the atmosphere over the scene colour c at this pixel (oaxatmos_fp.glsl)
vec3 Atmosphere(vec3 c, vec2 tc)
{
	float d = texture2D(u_ScreenDepthMap, tc).r;
	vec2 ndc = tc * 2.0 - 1.0;
	float zNear = u_ViewInfo.x;
	float zFar = u_ViewInfo.y;
	float zEye = d >= 0.99999 ? u_FogDistance.w
		: 2.0 * zNear * zFar / (zFar + zNear - (d * 2.0 - 1.0) * (zFar - zNear));
	vec3 dir = u_ViewForward - u_ViewLeft * (ndc.x * u_NormalScale.x) + u_ViewUp * (ndc.y * u_NormalScale.y);
	vec3 P = u_ViewOrigin + dir * zEye;
	float rayLen = zEye * length(dir);
	float f = max(u_FogDistance.x, 1e-6);
	float h0 = max(u_ViewOrigin.z - u_FogDistance.y, 0.0);
	float h1 = max(P.z - u_FogDistance.y, 0.0);
	float e0 = exp(-f * h0), e1 = exp(-f * h1);
	float dh = h1 - h0;
	float od = u_FogColorMask.a * rayLen * (abs(dh) > 0.5 ? (e0 - e1) / (f * dh) : e0);
	float amount = 1.0 - exp(-od);
	float sun = pow(max(dot(normalize(dir), u_PrimaryLightOrigin.xyz), 0.0), 8.0);
	return mix(c, u_FogColorMask.rgb * (1.0 + u_FogDistance.z * sun), amount);
}

const vec3  LUMINANCE_VECTOR =   vec3(0.2125, 0.7154, 0.0721); //vec3(0.299, 0.587, 0.114);

vec3 GetValues(vec2 offset, vec3 current)
{
	vec2 tc = var_TexCoords + u_InvTexRes * offset;
	vec3 minAvgMax = texture2D(u_TextureMap, tc).rgb;

#ifdef FIRST_PASS
	if (u_FogEyeT > 0.5)
		minAvgMax = Atmosphere(minAvgMax, tc);

  #if defined(USE_PBR)
	minAvgMax *= minAvgMax;
  #endif

	float lumi = max(dot(LUMINANCE_VECTOR, minAvgMax), 0.000001);
	float loglumi = clamp(log2(lumi), -10.0, 10.0);
	minAvgMax = vec3(loglumi * 0.05 + 0.5);
#endif

	return vec3(min(current.x, minAvgMax.x), current.y + minAvgMax.y, max(current.z, minAvgMax.z));
}

void main()
{
	vec3 current = vec3(1.0, 0.0, 0.0);

#ifdef FIRST_PASS
	current = GetValues(vec2( 0.0,  0.0), current);
#else
	current = GetValues(vec2(-1.5, -1.5), current);
	current = GetValues(vec2(-0.5, -1.5), current);
	current = GetValues(vec2( 0.5, -1.5), current);
	current = GetValues(vec2( 1.5, -1.5), current);
	
	current = GetValues(vec2(-1.5, -0.5), current);
	current = GetValues(vec2(-0.5, -0.5), current);
	current = GetValues(vec2( 0.5, -0.5), current);
	current = GetValues(vec2( 1.5, -0.5), current);
	
	current = GetValues(vec2(-1.5,  0.5), current);
	current = GetValues(vec2(-0.5,  0.5), current);
	current = GetValues(vec2( 0.5,  0.5), current);
	current = GetValues(vec2( 1.5,  0.5), current);

	current = GetValues(vec2(-1.5,  1.5), current);
	current = GetValues(vec2(-0.5,  1.5), current);
	current = GetValues(vec2( 0.5,  1.5), current);
	current = GetValues(vec2( 1.5,  1.5), current);

	current.y *= 0.0625;
#endif

	gl_FragColor = vec4(current, 1.0);
}
