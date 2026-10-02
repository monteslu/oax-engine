// oax view fog: distance fog from the scene depth buffer.
//   u_Color      fog rgb, density
//   u_ViewInfo   zNear, zFar, start, end
//   u_NormalScale  tan(fovX/2), tan(fovY/2), viewport x/w, viewport y/h in
//                  texture space (x, y, 1/w, 1/h packed as zw = origin)
//   u_SpecularScale viewport size in texture space (w, h)
// end > start: linear from start to end, reaching `density` (0..1).
// otherwise:   exponential, 1 - exp(-density * (distance - start)).
// Depth 1 (sky, nothing drawn) is infinitely far.
uniform sampler2D u_ScreenDepthMap;

uniform vec4   u_Color;
uniform vec4   u_ViewInfo;
uniform vec4   u_NormalScale;
uniform vec4   u_SpecularScale;

varying vec2   var_TexCoords;

void main()
{
	float d = texture2D(u_ScreenDepthMap, var_TexCoords).r;
	vec2 ndc = ((var_TexCoords - u_NormalScale.zw) / u_SpecularScale.xy) * 2.0 - 1.0;
	float zNear = u_ViewInfo.x;
	float zFar = u_ViewInfo.y;
	float f;

	if (d >= 0.99999)
	{
		f = u_ViewInfo.w > u_ViewInfo.z ? clamp(u_Color.a, 0.0, 1.0) : 1.0;
	}
	else
	{
		float zEye = 2.0 * zNear * zFar / (zFar + zNear - (d * 2.0 - 1.0) * (zFar - zNear));
		float dist = zEye * length(vec3(1.0, ndc.x * u_NormalScale.x, ndc.y * u_NormalScale.y));

		if (u_ViewInfo.w > u_ViewInfo.z)
			f = clamp((dist - u_ViewInfo.z) / (u_ViewInfo.w - u_ViewInfo.z), 0.0, 1.0) * clamp(u_Color.a, 0.0, 1.0);
		else
			f = 1.0 - exp(-u_Color.a * max(dist - u_ViewInfo.z, 0.0));
	}

	gl_FragColor = vec4(u_Color.rgb, f);
}
