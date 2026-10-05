# oax_canyon art

Models and textures used by `tests/maps/src/oax_canyon.mjs`. Every file's
source, author and licence is in `licenses/`; the map build copies those
into the map's package as `credits/oax_canyon/`.

## Trees (`models/oax/foliage`, `textures/oax_foliage`)

Generated with ez-tree (MIT; bark and needle textures CC0 from ambientCG and
Poly Haven), exported as glTF, then converted:

    node misc/tools/glb-to-md3.mjs eztree-ponderosa.glb   tests/maps/assets/canyon models/oax/foliage/conifer_1.md3 --textures textures/oax_foliage
    node misc/tools/glb-to-md3.mjs eztree-ponderosa-2.glb tests/maps/assets/canyon models/oax/foliage/conifer_2.md3 --textures textures/oax_foliage
    node misc/tools/glb-to-md3.mjs eztree-pinyon.glb      tests/maps/assets/canyon models/oax/foliage/conifer_3.md3 --textures textures/oax_foliage
    node misc/tools/glb-to-md3.mjs eztree-juniper.glb     tests/maps/assets/canyon models/oax/foliage/conifer_4.md3 --textures textures/oax_foliage
    node misc/tools/glb-to-md3.mjs eztree-snag.glb        tests/maps/assets/canyon models/oax/foliage/snag.md3 --textures textures/oax_foliage

The needle atlases (`conifer_*_1.png`) have their fully transparent texels
filled with the average needle colour, so the smaller mip levels do not
darken toward black:

    avg=$(convert in.png -channel A -threshold 50% +channel -scale 1x1! -alpha off -format '#%[hex:u]' info:)
    convert in.png -background "$avg" -alpha background in.png

## Grass (`textures/oax_canyon/grass_04.png`, `grass_05.png`)

Yughues grass packs (CC0): `grass_04` and `grass_05` with their opacity map
as alpha, brightened (`-modulate 125,135,100`) and colour-filled as above.

## Vehicles (`models/oax/vehicles`)

    # the six-wheeled carrier (pflat, CC-BY-SA 3.0): body without wheels, one wheel
    node misc/tools/glb-to-md3.mjs apc-textured.glb tests/maps/assets/canyon models/oax/vehicles/apc.md3 \
      --textures models/oax/vehicles --scale 28 --offset "-26.5 0 -52" --exclude '^wheel_'
    node misc/tools/glb-to-md3.mjs apc-textured.glb tests/maps/assets/canyon models/oax/vehicles/apc_wheel.md3 \
      --textures models/oax/vehicles --texname apc --scale 28 --offset "-100 48 -24" --only '^wheel_r01$'
    # its blue camo regraded to a grey-olive
    convert apc_0.png -modulate 80,12,100 -fill '#4f5236' -colorize 30% -sigmoidal-contrast 2,45% apc_0.png

    # the hover tank (KillGorack, CC0) in three parts, the turret turned to
    # face forward: the hull without the turret, the turret with its origin
    # at its pivot (the source model's origin: hull space -35 0 18), and the
    # barrel with its origin at its pitch pivot, 16 units ahead of that
    # (bg_vehicleTypes: gunMount, gunBarrelPivot)
    T='Barrel|Optics|Shield|Turret=30'
    node misc/tools/glb-to-md3.mjs hovertank-aegis-killgorack.glb tests/maps/assets/canyon models/oax/vehicles/hovertank.md3 \
      --textures models/oax/vehicles --scale 32 --offset "-35 0 18" --rotz "$T" --exclude '^(Barrel|Optics|Shield|Turret)$'
    node misc/tools/glb-to-md3.mjs hovertank-aegis-killgorack.glb tests/maps/assets/canyon models/oax/vehicles/hovertank_turret.md3 \
      --textures models/oax/vehicles --texname hovertank --scale 32 --offset "0 0 0" --rotz "$T" --only '^(Optics|Shield|Turret)$'
    node misc/tools/glb-to-md3.mjs hovertank-aegis-killgorack.glb tests/maps/assets/canyon models/oax/vehicles/hovertank_barrel.md3 \
      --textures models/oax/vehicles --texname hovertank --scale 32 --offset "-16 0 0" --rotz "$T" --only '^Barrel$'
    # its 4K hull texture halved and stored as JPEG (it is opaque); the
    # textures the converter writes are not copied over these

The scale and offset put each model's origin at the centre of its physics
hull (`bg_vehicleTypes` in the game code: types `apc` and `hovertank`).
