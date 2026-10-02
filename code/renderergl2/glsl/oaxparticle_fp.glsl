// oax stateless particles: the stage material's first texture times the
// particle colour, and soft particles: a depth fade against the opaque
// scene's depth (a copy, so the pass reads no buffer it draws to).
//   u_ViewInfo   zNear, zFar, soft distance (0 = off), soft mode
//                (1 fade alpha, 2 fade rgba, 3 fade toward white for
//                modulating blends)
//   u_InvTexRes  1 / scene depth size
uniform sampler2D u_DiffuseMap;
uniform sampler2D u_ScreenDepthMap;

uniform vec4   u_ViewInfo;
uniform vec2   u_InvTexRes;

varying vec2   var_TexCoords;
varying vec4   var_Color;
varying float  var_EyeZ;

void main()
{
	vec4 color = texture2D(u_DiffuseMap, var_TexCoords) * var_Color;

	if (u_ViewInfo.z > 0.0)
	{
		float zNear = u_ViewInfo.x;
		float zFar = u_ViewInfo.y;
		float d = texture2D(u_ScreenDepthMap, gl_FragCoord.xy * u_InvTexRes).r;
		float zScene = 2.0 * zNear * zFar / (zFar + zNear - (d * 2.0 - 1.0) * (zFar - zNear));
		float fade = clamp((zScene - var_EyeZ) / u_ViewInfo.z, 0.0, 1.0);

		if (u_ViewInfo.w == 1.0)
			color.a *= fade;
		else if (u_ViewInfo.w == 3.0)
			color = mix(vec4(1.0), color, fade);
		else
			color *= fade;
	}

	gl_FragColor = color;
}
