# oax_assault art

Models used by `tests/maps/src/oax_assault.mjs`. Every file's source, author
and licence is in `licenses/`; the map build copies those into the map's
package as `credits/oax_assault/`.

## The golden cow (`models/oax/cow`)

Quaternius's CC0 Farm Animals Pack cow (`OBJ/Cow.obj`), 130 units tall,
normals smoothed, one MD3 surface per material (`White`, `Black`, `Pink`):

    node misc/tools/obj-to-md3.mjs Cow.obj tests/maps/assets/assault/models/oax/cow/goldcow.md3 \
      --name models/oax/cow/goldcow.md3 --shader models/oax/cow/gold --height 130 --smooth

Its shaders (`gold_white`, `gold_black`, `gold_pink`
in the map's shader file) are `oaxMetal` gold (docs/materials.md) over a dark
base colour, reflecting the `misc_cubemap` probe the map places by the cow.

`goldshine.png` is the one texture: a gold sphere's hot spots (two lights
and the sky's rim, everything else black), drawn additively with
`tcGen environment` over the metal, as old games did chrome. The sphere was
rendered in Blender (a metallic gold sphere, an orthographic camera, a sky
gradient and two area lights, transparent background), then:

    convert -size 1x256 gradient:black-white \( -size 1x1 xc:'#000000' xc:'#000000' xc:'#6a4a10' \
      xc:'#ffcf6a' xc:'#fff6dc' +append -filter triangle -resize 256x1! \) -delete 0 hiramp.png
    convert goldsphere.png -background black -alpha remove -alpha off -colorspace gray \
      -level 85%,100%,1.0 hiramp.png -clut -blur 0x3 goldshine.png
