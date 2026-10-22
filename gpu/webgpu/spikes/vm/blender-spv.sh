#!/bin/bash
# Spike S2 input: Blender 4.3.2 (Debian trixie arm64) on zink on lavapipe under Xvfb; zink dumps every SPIR-V module it makes.
#   inside the chroot: bash /out/blender-spv.sh MODE   (MODE = eevee | workbench | ui)
mode=${1:-eevee}; prof=${2:-}; D=/out/spv-$mode${prof:+-$prof}; rm -rf $D; mkdir -p $D; cd $D
Xvfb :7 -screen 0 1280x800x24 >/dev/null 2>&1 &
xp=$!; sleep 2
[ -n "$prof" ] && . /out/wgmask/$prof-env.sh && export MESA_GL_VERSION_OVERRIDE=4.3 MESA_GLSL_VERSION_OVERRIDE=430
export DISPLAY=:7 MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink LIBGL_ALWAYS_SOFTWARE=1 ZINK_DEBUG=spirv
case $mode in
eevee) timeout 900 blender -b --factory-startup -E BLENDER_EEVEE_NEXT -o $D/render_ -F PNG -f 1 > $D/blender.log 2>&1 ;;
workbench) timeout 600 blender -b --factory-startup -E BLENDER_WORKBENCH -o $D/render_ -F PNG -f 1 > $D/blender.log 2>&1 ;;
ui) timeout 600 blender --factory-startup --python-expr 'import bpy
def q():
    w = bpy.context.window_manager.windows[0]
    with bpy.context.temp_override(window=w, screen=w.screen, area=w.screen.areas[0]):
        bpy.ops.screen.screenshot(filepath="'$D'/ui.png")
    bpy.ops.wm.quit_blender()
bpy.app.timers.register(q, first_interval=60)' > $D/blender.log 2>&1 ;;
esac
echo "blender rc $?"
kill $xp
ls $D | grep -c spv; tail -5 $D/blender.log; ls $D | grep -v spv
