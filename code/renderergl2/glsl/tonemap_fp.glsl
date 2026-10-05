#if defined(OAX_MSAA_SAMPLES)
// oax: the multisample render target itself, resolved here (the average of
// its samples, as the resolve blit), so the frame needs no resolve pass
uniform sampler2DMS u_TextureMap;
#else
uniform sampler2D u_TextureMap;
#endif
uniform sampler2D u_LevelsMap;

uniform vec4      u_Color;


uniform vec2      u_AutoExposureMinMax;
uniform vec3      u_ToneMinAvgMaxLinear;
uniform float     u_Gamma;

// oax, when the post-process writes straight to the screen (tr_oax_env.c):
// colour grading (worldspawn oax_grade): u_FogDepth saturation, contrast,
// vignette, on; u_DirectedLight the colour multiplier
uniform vec4      u_FogDepth;
uniform vec3      u_DirectedLight;
// and the atmosphere (worldspawn oax_atmosphere), as oaxatmos_fp.glsl:
// u_FogEyeT > 0.5 on; u_FogColorMask rgb + density; u_FogDistance falloff,
// base z, sun scatter, sky distance
#if defined(OAX_MSAA_SAMPLES)
uniform sampler2DMS u_ScreenDepthMap;
#else
uniform sampler2D u_ScreenDepthMap;
#endif
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
#if defined(OAX_MSAA_SAMPLES)
	float d = texelFetch(u_ScreenDepthMap, ivec2(gl_FragCoord.xy), 0).r;	// sample 0, as a nearest depth resolve
#else
	float d = texture2D(u_ScreenDepthMap, tc).r;
#endif
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

varying vec2      var_TexCoords;
varying float     var_InvWhite;

const vec3  LUMINANCE_VECTOR =   vec3(0.2125, 0.7154, 0.0721); //vec3(0.299, 0.587, 0.114);

float FilmicTonemap(float x)
{
	const float SS  = 0.22; // Shoulder Strength
	const float LS  = 0.30; // Linear Strength
	const float LA  = 0.10; // Linear Angle
	const float TS  = 0.20; // Toe Strength
	const float TAN = 0.01; // Toe Angle Numerator
	const float TAD = 0.30; // Toe Angle Denominator

	return ((x*(SS*x+LA*LS)+TS*TAN)/(x*(SS*x+LS)+TS*TAD)) - TAN/TAD;
}

void main()
{
#if defined(OAX_MSAA_SAMPLES)
	vec4 color = vec4(0.0);
	for (int i = 0; i < OAX_MSAA_SAMPLES; i++)
		color += texelFetch(u_TextureMap, ivec2(gl_FragCoord.xy), i);
	color /= float(OAX_MSAA_SAMPLES);
#else
	vec4 color = texture2D(u_TextureMap, var_TexCoords);
#endif
	if (u_FogEyeT > 0.5)
		color.rgb = Atmosphere(color.rgb, var_TexCoords);
	color *= u_Color;

#if defined(USE_PBR)
	color.rgb *= color.rgb;
#endif

	vec3 minAvgMax = texture2D(u_LevelsMap, var_TexCoords).rgb;
	vec3 logMinAvgMaxLum = clamp(minAvgMax * 20.0 - 10.0, -u_AutoExposureMinMax.y, -u_AutoExposureMinMax.x);

	float invAvgLum = u_ToneMinAvgMaxLinear.y * exp2(-logMinAvgMaxLum.y);

	color.rgb = color.rgb * invAvgLum - u_ToneMinAvgMaxLinear.xxx;
	color.rgb = max(vec3(0.0), color.rgb);

	color.r = FilmicTonemap(color.r);
	color.g = FilmicTonemap(color.g);
	color.b = FilmicTonemap(color.b);

	color.rgb = clamp(color.rgb * var_InvWhite, 0.0, 1.0);

	// r_gamma, applied here when the display has no hardware gamma ramp
	color.rgb = pow(color.rgb, vec3(1.0 / u_Gamma));

#if defined(USE_PBR)
	color.rgb = sqrt(color.rgb);
#endif

	if (u_FogDepth.w > 0.5)
	{
		vec3 c = color.rgb;
		float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
		c = mix(vec3(lum), c, u_FogDepth.x);
		c = (c - 0.5) * u_FogDepth.y + 0.5;
		c *= u_DirectedLight;
		vec2 q = var_TexCoords - 0.5;
		float v = 1.0 - u_FogDepth.z * smoothstep(0.25, 0.75, length(q) * 1.414);
		color.rgb = clamp(c * v, 0.0, 1.0);
	}

	// add a bit of dither to reduce banding
	color.rgb += vec3(1.0/510.0 * mod(gl_FragCoord.x + gl_FragCoord.y, 2.0) - 1.0/1020.0);

	gl_FragColor = color;
}
