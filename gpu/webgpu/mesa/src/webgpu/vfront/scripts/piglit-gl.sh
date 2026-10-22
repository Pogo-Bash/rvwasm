#!/bin/bash
# In the chroot: the GL 1.x-3.3 piglit selection vfront is measured with, through vfront and through vrend;
# then the comparison.   piglit-gl.sh [JOBS]
SEL='spec@(glsl-1\.(10|20|30|40|50)@execution|glsl-3\.30|!opengl [123]|ext_transform_feedback|arb_uniform_buffer_object|arb_texture_buffer_object|ext_framebuffer_object|arb_framebuffer_object|arb_instanced_arrays|arb_draw_indirect|arb_shader_storage_buffer_object|arb_compute_shader|arb_texture_multisample|arb_sample_shading|arb_occlusion_query|arb_timer_query)'
j=${1:-4}
bash /t6/vfront/scripts/piglit-run.sh gl-vfront quick "$SEL" $j | grep -E "rc|pass|fail|crash|skip|timeout|total"
[ -n "$NO_VREND" ] || SERVER=vrend bash /t6/vfront/scripts/piglit-run.sh gl-vrend quick "$SEL" $j | grep -E "rc|pass|fail|crash|skip|timeout|total"
python3 /t6/vfront/tools/pgcmp.py /t6/piglit/gl-vrend /t6/piglit/gl-vfront 40
