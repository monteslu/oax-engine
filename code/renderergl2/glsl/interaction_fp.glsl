// interaction_fp.glsl: unified lighting (phase 5) fragment shader, written
// for this engine from the id Tech 4 interaction parameter list.
//
//   projection = projImage(S/Q, T/Q) * falloffImage(F, 0.5)   (zero outside)
//   diffuse    = diffuseMap * diffuseColor * max(N.L, 0)
//   specular   = specularMap * specularColor * pow(max(N.H, 0), 16) * 2
//   color      = (diffuse + specular) * projection * lightColor * shadow
//
// The 16 / 2 specular constants follow id Tech 4's specular table image.
// Defines: ULIGHT_DEPTH (shadow map), ULIGHT_AMBIENT (diffuse * ambient),
// USE_SHADOW_CUBE (point lights), USE_SHADOW_2D (projected and parallel
// lights), SWIZZLE_NORMALMAP.

#if defined(ULIGHT_DEPTH)

void main()
{
	gl_FragColor = vec4(0.0);
}

#else

uniform sampler2D u_DiffuseMap;
uniform vec4      u_DiffuseColor;

varying vec2      var_TexCoords;
varying vec3      var_Position;
varying vec3      var_Normal;
varying vec4      var_Tangent;

#if defined(ULIGHT_AMBIENT)

uniform vec3      u_AmbientLight;

void main()
{
	vec4 diffuse = texture(u_DiffuseMap, var_TexCoords) * u_DiffuseColor;
	gl_FragColor = vec4(diffuse.rgb * u_AmbientLight, 1.0);
}

#else

uniform sampler2D u_NormalMap;
uniform sampler2D u_SpecularMap;
uniform sampler2D u_LightProjMap;
uniform sampler2D u_LightFalloffMap;

uniform vec4      u_LightProjS;
uniform vec4      u_LightProjT;
uniform vec4      u_LightProjQ;
uniform vec4      u_LightFalloffS;
uniform vec3      u_LightOriginW;
uniform vec3      u_LightColor;
uniform vec3      u_ViewOrigin;
uniform vec4      u_SpecularColor;
uniform vec4      u_NormalScale;
uniform vec4      u_ShadowParams;   // near (0: orthographic), far, depth bias (fraction), normal offset

#if defined(USE_SHADOW_CUBE)
uniform samplerCubeShadow u_ShadowCube;
#elif defined(USE_SHADOW_2D)
uniform sampler2DShadow u_Shadow2D;
uniform mat4      u_ShadowMatrix;
#endif

// perspective depth of a point at distance m along the shadow view axis,
// pulled toward the light by a fraction of m (the depth bias)
float ShadowDepth(float m)
{
	float n = u_ShadowParams.x, f = u_ShadowParams.y;
	m *= 1.0 - u_ShadowParams.z;
	return 0.5 * ((f + n) / (f - n) - (2.0 * f * n) / ((f - n) * m)) + 0.5;
}

float Shadow(vec3 position, vec3 normal)
{
#if defined(USE_SHADOW_CUBE)
	// offset along the normal by about a shadow map texel at this distance
	vec3 d = position - u_LightOriginW;
	float dist = max(max(abs(d.x), abs(d.y)), abs(d.z));
	position += normal * (u_ShadowParams.w * dist);
	d = position - u_LightOriginW;
	float m = max(max(abs(d.x), abs(d.y)), abs(d.z));
	return texture(u_ShadowCube, vec4(d, ShadowDepth(m)));
#elif defined(USE_SHADOW_2D)
	// rows: S, T, depth, Q; u_ShadowParams.x == 0 marks an orthographic map
	vec4 sc = u_ShadowMatrix * vec4(position, 1.0);
	position += normal * (u_ShadowParams.w * (u_ShadowParams.x > 0.0 ? sc.w : 1.0));
	sc = u_ShadowMatrix * vec4(position, 1.0);
	float depth;
	if (u_ShadowParams.x > 0.0)
	{
		sc.xy /= sc.w;
		depth = ShadowDepth(sc.w);
	}
	else
	{
		depth = sc.z - u_ShadowParams.z;
	}
	if (sc.x <= 0.0 || sc.x >= 1.0 || sc.y <= 0.0 || sc.y >= 1.0)
		return 1.0;
	return texture(u_Shadow2D, vec3(sc.xy, depth));
#else
	return 1.0;
#endif
}

void main()
{
	vec4 P = vec4(var_Position, 1.0);
	vec4 lp = vec4(dot(P, u_LightProjS), dot(P, u_LightProjT), dot(P, u_LightProjQ), dot(P, u_LightFalloffS));

	// zero outside the light volume (id Tech 4 clamps these images to zero)
	if (lp.z <= 0.0)
		discard;
	vec2 st = lp.xy / lp.z;
	if (st.x < 0.0 || st.x > 1.0 || st.y < 0.0 || st.y > 1.0 || lp.w < 0.0 || lp.w > 1.0)
		discard;

	vec3 proj = texture(u_LightProjMap, st).rgb * texture(u_LightFalloffMap, vec2(lp.w, 0.5)).rgb;

	// Q3 winds front faces the other way round: same test as lightall_fp
	vec3 surfNormal = normalize(gl_FrontFacing ? -var_Normal : var_Normal);
	vec3 tangent = normalize(var_Tangent.xyz - surfNormal * dot(surfNormal, var_Tangent.xyz));
	vec3 bitangent = cross(surfNormal, tangent) * (var_Tangent.w < 0.0 ? -1.0 : 1.0);

	vec3 N;
#if defined(SWIZZLE_NORMALMAP)
	N.xy = texture(u_NormalMap, var_TexCoords).ag - vec2(0.5);
#else
	N.xy = texture(u_NormalMap, var_TexCoords).rg - vec2(0.5);
#endif
	N.xy *= u_NormalScale.xy;
	N.z = sqrt(clamp((0.25 - N.x * N.x) - N.y * N.y, 0.0, 1.0));
	N = normalize(mat3(tangent, bitangent, surfNormal) * N);

	vec3 L = normalize(u_LightOriginW - var_Position);
	vec3 V = normalize(u_ViewOrigin - var_Position);
	vec3 H = normalize(L + V);

	float NL = max(dot(N, L), 0.0);
	float NH = max(dot(N, H), 0.0);
	float surfNL = dot(surfNormal, L);

	vec4 diffuse = texture(u_DiffuseMap, var_TexCoords) * u_DiffuseColor;
	vec3 color = diffuse.rgb * NL;
	color += texture(u_SpecularMap, var_TexCoords).rgb * u_SpecularColor.rgb * (pow(NH, 16.0) * 2.0 * step(0.0, surfNL));

	float shadow = 1.0;
	if (surfNL > 0.0)
		shadow = Shadow(var_Position, surfNormal);

	gl_FragColor = vec4(color * proj * u_LightColor * shadow, 1.0);
}

#endif
#endif
