// oax instanced foliage (tr_terrain.c): a unit mesh per instance
attribute vec3 attr_Position;
attribute vec3 attr_Normal;
attribute vec4 attr_TexCoord0;
attribute vec4 attr_Position2;   // per instance: origin xyz, scale
attribute vec4 attr_Normal2;     // per instance: cos yaw, sin yaw, random, kind

uniform mat4   u_ModelViewProjectionMatrix;
uniform vec3   u_ViewOrigin;
uniform vec4   u_Fade;           // start, 1 / (end - start), z: fade on

varying vec2   var_Tex;
varying float  var_Fade;
#if !defined(FOLIAGE_DEPTH)
varying vec3   var_Normal;
varying float  var_Height;
#endif

invariant gl_Position;

void main()
{
	vec2 cs = attr_Normal2.xy;
	vec3 p = attr_Position * attr_Position2.w;
	vec3 world = vec3(p.x * cs.x - p.y * cs.y, p.x * cs.y + p.y * cs.x, p.z) + attr_Position2.xyz;
	gl_Position = u_ModelViewProjectionMatrix * vec4(world, 1.0);
	var_Tex = attr_TexCoord0.st;
	// fade by distance to the instance root (whole instance fades together)
	var_Fade = u_Fade.z * clamp((distance(attr_Position2.xyz, u_ViewOrigin) - u_Fade.x) * u_Fade.y, 0.0, 1.0);
#if !defined(FOLIAGE_DEPTH)
	vec3 n = attr_Normal;
	var_Normal = vec3(n.x * cs.x - n.y * cs.y, n.x * cs.y + n.y * cs.x, n.z);
	var_Height = attr_Position.z;
#endif
}
