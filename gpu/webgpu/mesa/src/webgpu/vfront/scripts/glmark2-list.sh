# glmark2-es2's default scenes, 1 s each (sourced by run scripts: $GLMARK_ARGS)
D=${GLMARK_DURATION:-1}
GLMARK_ARGS="-s 400x300"
for b in build:use-vbo=false build:use-vbo=true texture:texture-filter=nearest texture:texture-filter=linear \
	texture:texture-filter=mipmap shading:shading=gouraud shading:shading=blinn-phong-inf shading:shading=phong \
	shading:shading=cel bump:bump-render=high-poly bump:bump-render=normals bump:bump-render=height \
	"effect2d:kernel=0,1,0;1,-4,1;0,1,0;" "effect2d:kernel=1,1,1,1,1;1,1,1,1,1;1,1,1,1,1;" \
	pulsar:light=false:quads=5:texture=false \
	desktop:blur-radius=5:effect=blur:passes=1:separable=true:windows=4 desktop:effect=shadow:windows=4 \
	buffer:columns=200:interleave=false:update-dispersion=0.9:update-fraction=0.5:update-method=map \
	buffer:columns=200:interleave=false:update-dispersion=0.9:update-fraction=0.5:update-method=subdata \
	buffer:columns=200:interleave=true:update-dispersion=0.9:update-fraction=0.5:update-method=map \
	ideas:speed=duration jellyfish terrain shadow refract \
	conditionals:fragment-steps=0:vertex-steps=0 conditionals:fragment-steps=5:vertex-steps=0 \
	function:fragment-complexity=low:fragment-steps=5 function:fragment-complexity=medium:fragment-steps=5 \
	loop:fragment-loop=false:fragment-steps=5:vertex-steps=5 loop:fragment-steps=5:fragment-uniform=false:vertex-steps=5; do
	GLMARK_ARGS="$GLMARK_ARGS -b $b:duration=$D"
done
