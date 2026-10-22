#!/bin/bash
# inside the chroot: bash /out/t2/blender-run.sh MODE TAG  (MODE = workbench | eevee | passes | ui)
# Blender 4.3.2 under Xvfb, as spike S1 ran it; output in $OUT/TAG-MODE (OUT defaults to /out/t2) (SAMPLES=n: EEVEE samples; SHADOW_POOL=MB)
mode=${1:-workbench}; tag=${2:-x}; O=${OUT:-/out/t2}; D=$O/$tag-$mode; rm -rf $D; mkdir -p $D; cd $D
disp=:$((20 + RANDOM % 50))
Xvfb $disp -screen 0 1280x800x24 >/dev/null 2>&1 &
xp=$!; sleep 2
export DISPLAY=$disp
t0=$(date +%s.%N)
case $mode in
eevee) pre=(); [ -n "$SAMPLES" ] && pre+=(--python-expr "import bpy; bpy.context.scene.eevee.taa_render_samples = $SAMPLES")
	[ -n "$SHADOW_POOL" ] && pre+=(--python-expr "import bpy; bpy.context.scene.eevee.shadow_pool_size = '$SHADOW_POOL'")
	timeout ${TMO:-1500} blender -b --factory-startup -E BLENDER_EEVEE_NEXT "${pre[@]}" -o $D/render_ -F PNG -f 1 > $D/blender.log 2>&1 ;;
passes) PASSDIR=$D SAMPLES=${SAMPLES:-1} timeout ${TMO:-1500} blender -b --factory-startup -E BLENDER_EEVEE_NEXT --python $O/passes.py -o $D/render_ -F PNG -f 1 > $D/blender.log 2>&1 ;;
workbench) timeout ${TMO:-900} blender -b --factory-startup -E BLENDER_WORKBENCH -o $D/render_ -F PNG -f 1 > $D/blender.log 2>&1 ;;
ui) timeout ${TMO:-900} blender --factory-startup --python-expr 'import bpy, time
t = [time.time()]
def q():
    w = bpy.context.window_manager.windows[0]
    with bpy.context.temp_override(window=w, screen=w.screen, area=w.screen.areas[0]):
        bpy.ops.screen.screenshot(filepath="'$D'/ui.png")
    bpy.ops.wm.quit_blender()
bpy.app.timers.register(q, first_interval='${UI_WAIT:-60}')' > $D/blender.log 2>&1 ;;
esac
rc=$?
t1=$(date +%s.%N)
echo "blender rc $rc in $(python3 -c "print(round($t1 - $t0, 1))") s"
kill $xp 2>/dev/null
tail -4 $D/blender.log; ls $D
