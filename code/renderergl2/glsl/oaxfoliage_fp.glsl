// oax instanced foliage: alpha tested, distance fade as an ordered dither
uniform sampler2D u_Tex;
#if !defined(FOLIAGE_DEPTH)
uniform sampler2D u_ScreenShadow;
uniform vec3   u_SunDir;
uniform vec3   u_SunColor;
uniform vec3   u_Ambient;
uniform vec4   u_ScreenInfo;
varying vec3   var_Normal;
varying float  var_Height;
#endif

varying vec2   var_Tex;
varying float  var_Fade;

// 4x4 Bayer threshold in (0, 1)
float Bayer4(vec2 fc)
{
	int x = int(mod(fc.x, 4.0));
	int y = int(mod(fc.y, 4.0));
	int a = (x ^ y) & 1, b = (x ^ y) >> 1 & 1, c = y & 1, d = y >> 1 & 1;
	int v = a * 8 + c * 4 + b * 2 + d;
	return (float(v) + 0.5) / 16.0;
}

void main()
{
	vec4 tex = texture2D(u_Tex, var_Tex);
	if (tex.a < 0.5 || var_Fade >= Bayer4(gl_FragCoord.xy))
		discard;
#if defined(FOLIAGE_DEPTH)
	gl_FragColor = vec4(0.0);
#else
	vec3 n = normalize(var_Normal);
	float ndl = max(dot(n, u_SunDir), 0.0) * 0.7 + 0.3;
	float shadow = 1.0;
	if (u_ScreenInfo.w > 0.5)
		shadow = texture2D(u_ScreenShadow, gl_FragCoord.xy * u_ScreenInfo.xy).r;
	// a little self-shadow toward the base
	float ao = 0.55 + 0.45 * clamp(var_Height * 1.5, 0.0, 1.0);
	vec3 light = (u_Ambient + u_SunColor * (ndl * shadow)) * ao;
	gl_FragColor = vec4(tex.rgb * light, 1.0);
#endif
}
