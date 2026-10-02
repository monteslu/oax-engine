// oax heightmap terrain (tr_terrain.c): world-space vertices, so no model matrix
attribute vec3 attr_Position;
attribute vec3 attr_Normal;

uniform mat4   u_ModelViewProjectionMatrix;

#if !defined(TERRAIN_DEPTH)
varying vec3   var_Position;
varying vec3   var_Normal;
#endif

// the depth prepass and the colour pass must produce the same depths
invariant gl_Position;

void main()
{
	gl_Position = u_ModelViewProjectionMatrix * vec4(attr_Position, 1.0);
#if !defined(TERRAIN_DEPTH)
	var_Position = attr_Position;
	var_Normal = attr_Normal;
#endif
}
