uniform sampler2D u_DiffuseMap;

uniform int       u_AlphaTest;

varying vec2      var_DiffuseTex;

varying vec4      var_Color;

// oax detailFade (docs/materials.md): start, 1 / (end - start), the
// blend's neutral value (< 0: fade alpha), on. The fade runs on view depth
// (eye-space z, 1 / gl_FragCoord.w), as UE1 fades detail textures: exact
// per pixel on any polygon, however large.
uniform vec4      u_OaxDetailFade;
// per-vertex mode (w 2): the fade interpolated from the vertices
varying float     var_OaxFade;


void main()
{
	vec4 color  = texture2D(u_DiffuseMap, var_DiffuseTex);

	float alpha = color.a * var_Color.a;
	if (u_AlphaTest == 1)
	{
		if (alpha == 0.0)
			discard;
	}
	else if (u_AlphaTest == 2)
	{
		if (alpha >= 0.5)
			discard;
	}
	else if (u_AlphaTest == 3)
	{
		if (alpha < 0.5)
			discard;
	}
	
	gl_FragColor.rgb = color.rgb * var_Color.rgb;
	gl_FragColor.a = alpha;

	if (u_OaxDetailFade.w > 0.5)
	{
		float near = u_OaxDetailFade.w > 1.5 ? var_OaxFade
			: 1.0 - clamp((1.0 / gl_FragCoord.w - u_OaxDetailFade.x) * u_OaxDetailFade.y, 0.0, 1.0);
		if (u_OaxDetailFade.z >= 0.0)
			gl_FragColor.rgb = mix(vec3(u_OaxDetailFade.z), gl_FragColor.rgb, near);
		else
			gl_FragColor.a *= near;
	}
}
