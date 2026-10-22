#!/bin/bash
# In the chroot, against a running vfront-vtest ($VTEST_SOCKET_NAME; main-run.sh sets it): the GL version the
# guest's virgl driver reports.
#   main-run.sh --vk wgvk NAME 'bash /t6/vfront/scripts/gl-info.sh'
# eglinfo (surfaceless) prints the core and compatibility profiles. GLX: glxinfo fails with an X_GetImage BadMatch
# on virpipe (its window is never mapped; virgl_test_server's own virpipe does the same, and dies), so glxgears
# -info (a mapped window) prints the compatibility profile's version through GLX.
export GALLIUM_DRIVER=virpipe LIBGL_ALWAYS_SOFTWARE=1
echo "=== eglinfo (surfaceless): core profile"
EGL_PLATFORM=surfaceless eglinfo -p surfaceless 2>&1 | grep -E "OpenGL (core|compat|ES)|error|Error" | head -12
Xvfb :7 -screen 0 1024x768x24 -nolisten tcp > /t6/log/xvfb.log 2>&1 &
xp=$!
for i in $(seq 600); do [ -S /tmp/.X11-unix/X7 ] && break; sleep 0.2; done
echo "=== glxgears -info (GLX, Xvfb)"
DISPLAY=:7 timeout 20 glxgears -info 2>&1 | grep -E "GL_(RENDERER|VERSION)|rror" | head -5
kill $xp 2>/dev/null
